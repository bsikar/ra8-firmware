/**
 * @file ra_lvd.h
 * @brief Programmable Voltage Detection (PVD / LVD) HAL driver
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Full RA8D2 PVD coverage per HUM Ch 8 "Programmable Voltage
 * Detection (PVD)", p 300-316 (R01UH1065EJ rev 1.30). The HUM uses
 * the PVD name; the project keeps the FSP-style "lvd" short name for
 * symmetry with `r_lvd` and the existing `ra8d2_lvd_regs.h`.
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
 *      event-source select belongs in `ra_elc`.
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
 * callers must wrap each driver call with `ra_pwr_unlock_pvd()` /
 * `ra_pwr_lock_pvd()`. In unit tests the host sim mmap has no lock so
 * the writes always land.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8d2_lvd_regs.h"
#include "ra_err.h"

/**
 * @enum ra_lvd_edge_t
 * @brief PVDmCR1.IDTSEL[1:0] edge selector for monitor m channels.
 *
 * @details
 * From HUM 8.2.6 p 307:
 *   - 00b = rise (when VCC >= Vdetm is detected).
 *   - 01b = fall (when VCC <  Vdetm is detected).
 *   - 10b = both (any crossing fires DET).
 *   - 11b = "Settings prohibited" -- the driver rejects this.
 */
typedef enum : uint8_t {
  k_ra_lvd_edge_rise = 0U, /**< Detect VCC >= Vdetm rising crossings.  */
  k_ra_lvd_edge_fall = 1U, /**< Detect VCC <  Vdetm falling crossings. */
  k_ra_lvd_edge_both = 2U, /**< Detect either crossing.                */
} ra_lvd_edge_t;

/**
 * @enum ra_lvd_irq_type_t
 * @brief PVDmCR1.IRQSEL value -- NMI vs maskable interrupt.
 */
typedef enum : uint8_t {
  k_ra_lvd_irq_nmi      = 0U, /**< Non-maskable interrupt (default). */
  k_ra_lvd_irq_maskable = 1U, /**< Maskable interrupt via ICU.       */
} ra_lvd_irq_type_t;

/**
 * @enum ra_lvd_response_t
 * @brief Detection response selector for `ra_lvd_cfg_t::response`.
 *
 * @details
 * Determines which downstream signal the comparator drives:
 *   - none      -- comparator output enabled but neither IRQ nor reset
 *                  is asserted (only the MON / DET flags + ELC event
 *                  are useful in this mode -- HUM Section 8.7 p 315).
 *   - interrupt -- maskable IRQ via the ICU (m channels only). Sets
 *                  PVDmCR1.IRQSEL = 1 and PVDmCR0.RIE = 1.
 *   - nmi       -- non-maskable IRQ via the NMIER block (m channels
 *                  only). Sets PVDmCR1.IRQSEL = 0 and PVDmCR0.RIE = 1.
 *   - reset     -- LVD-style "fall to below Vdetm -> reset" path
 *                  (m: PVDmCR0.RI=1 + RIE=1; n: PVDnCR0.RE=1).
 *   - reset_on_rise -- HVD-style "rise above Vdetm -> reset" path,
 *                  forces PVDmFCR.RHSEL = 1 (HUM 8.2.8 p 308 "When
 *                  PVD reset generated by the VCC-rise detection is
 *                  required, you should clear RHSEL to 1").
 */
typedef enum : uint8_t {
  k_ra_lvd_response_none          = 0U, /**< Flags / ELC only.          */
  k_ra_lvd_response_interrupt     = 1U, /**< Maskable IRQ (m only).     */
  k_ra_lvd_response_nmi           = 2U, /**< NMI            (m only).   */
  k_ra_lvd_response_reset         = 3U, /**< LVD reset      (any).      */
  k_ra_lvd_response_reset_on_rise = 4U, /**< HVD reset      (any).      */
} ra_lvd_response_t;

/**
 * @enum ra_lvd_negate_t
 * @brief PVDmCR0.RN value -- reset deassertion timing.
 *
 * @details
 * From HUM 8.2.4 p 305 "RN bit":
 *   - case PVDmFCR.RHSEL = 0:
 *       0 = negation follows tPVDm after VCC > Vdetm.
 *       1 = negation follows tPVDm after the PVDm reset asserts.
 *   - case PVDmFCR.RHSEL = 1: RN = 1 is prohibited.
 *
 * The driver uses ``k_ra_lvd_negate_after_voltage`` as the safe default
 * because that is the only value compatible with Software Standby and
 * Deep Software Standby (HUM 8.2.4 p 306 "RN bit").
 */
typedef enum : uint8_t {
  k_ra_lvd_negate_after_voltage = 0U, /**< Negate after VCC clears Vdetm. */
  k_ra_lvd_negate_after_assert  = 1U, /**< Negate after reset asserts.    */
} ra_lvd_negate_t;

/**
 * @enum ra_lvd_hysteresis_t
 * @brief PVDmFCR.RHSEL / PVDnFCR.RHSEL value.
 *
 * @details
 * From HUM 8.2.8 p 308 and HUM 8.2.9 p 309 (identical semantics):
 *   - 0 -> hysteresis sits **above** Vdetm (the comparator behaves as
 *     a fall-detector with rising hysteresis -- LVD mode).
 *   - 1 -> hysteresis sits **below** Vdetm (rise-detector with falling
 *     hysteresis -- HVD mode). Required for ``reset_on_rise``.
 */
typedef enum : uint8_t {
  k_ra_lvd_hysteresis_lvd = 0U, /**< RHSEL = 0 (fall detect).  */
  k_ra_lvd_hysteresis_hvd = 1U, /**< RHSEL = 1 (rise detect).  */
} ra_lvd_hysteresis_t;

/**
 * @enum ra_lvd_pvdlvl_t
 * @brief PVDLVL[4:0] threshold encoding for PVDmCMPCR / PVDnCMPCR.
 *
 * @details
 * The legal range is 0x03..0x0F per HUM Ch 8.2.2 p 303 (Table 8.2).
 * Encodings 0x00..0x02 and 0x10..0x1F are reserved / prohibited; the
 * driver rejects any value outside
 * `k_ra_lvd_pvdlvl_min..k_ra_lvd_pvdlvl_max`. Each named entry maps
 * to a nominal Vdetm voltage; values verified against FSP
 * `lvd_threshold_t` for RA8D2 in `bsp/mcu/ra8d2/bsp_override.h`.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra_lvd_pvdlvl_min   = 0x03U, /**< Lowest legal threshold encoding.  */
  k_ra_lvd_pvdlvl_3_86v = 0x03U, /**< 3.86 V.                            */
  k_ra_lvd_pvdlvl_3_14v = 0x04U, /**< 3.14 V.                            */
  k_ra_lvd_pvdlvl_3_10v = 0x05U, /**< 3.10 V.                            */
  k_ra_lvd_pvdlvl_3_08v = 0x06U, /**< 3.08 V.                            */
  k_ra_lvd_pvdlvl_2_85v = 0x07U, /**< 2.85 V.                            */
  k_ra_lvd_pvdlvl_2_83v = 0x08U, /**< 2.83 V.                            */
  k_ra_lvd_pvdlvl_2_80v = 0x09U, /**< 2.80 V.                            */
  k_ra_lvd_pvdlvl_2_62v = 0x0AU, /**< 2.62 V.                            */
  k_ra_lvd_pvdlvl_2_33v = 0x0BU, /**< 2.33 V.                            */
  k_ra_lvd_pvdlvl_1_90v = 0x0CU, /**< 1.90 V.                            */
  k_ra_lvd_pvdlvl_1_86v = 0x0DU, /**< 1.86 V.                            */
  k_ra_lvd_pvdlvl_1_74v = 0x0EU, /**< 1.74 V.                            */
  k_ra_lvd_pvdlvl_1_71v = 0x0FU, /**< 1.71 V.                            */
  k_ra_lvd_pvdlvl_max   = 0x0FU, /**< Highest legal threshold encoding. */
} ra_lvd_pvdlvl_t;

/**
 * @enum ra_lvd_loco_div_t
 * @brief PVDmCR0.FSAMP[1:0] / PVDnCR0.FSAMP[1:0] sampling-clock divider.
 *
 * @details
 * Selects the LOCO divider feeding the digital filter. HUM Ch 8.2.4 p 305
 * "FSAMP[1:0] bits" documents 0..3 as the only legal range. The
 * encoding matches FSP `lvd_sample_clock_t`:
 *
 *   - 0 = LOCO / 2  (fastest, highest power).
 *   - 1 = LOCO / 4.
 *   - 2 = LOCO / 8.
 *   - 3 = LOCO / 16 (slowest, lowest power).
 *
 * The "2s + 3 LOCO cycles" stabilisation delay computed by
 * `ra_lvd_filter_delay_us` uses ``s = 2^(div+1)`` -- so div=0 implies
 * s=2 (LOCO/2), div=3 implies s=16 (LOCO/16).
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra_lvd_loco_div_2   = 0U, /**< LOCO / 2  (fastest, highest power). */
  k_ra_lvd_loco_div_4   = 1U, /**< LOCO / 4.                            */
  k_ra_lvd_loco_div_8   = 2U, /**< LOCO / 8.                            */
  k_ra_lvd_loco_div_16  = 3U, /**< LOCO / 16 (slowest, lowest power).  */
  k_ra_lvd_loco_div_max = 3U, /**< Highest legal value (sentinel).     */
} ra_lvd_loco_div_t;

/**
 * @enum ra_lvd_channel_t
 * @brief Voltage-monitor channel selector.
 *
 * @details
 * The RA8D2 PVD block defines four monitor channels: PVD1 / PVD2 are
 * the "m" series (NMI-capable, full feature set), PVD4 / PVD5 are the
 * "n" series (reset-only, no IRQ). Channels 0 and 3 are not exposed
 * on this part. See HUM Ch 8.1 Table 8.1 p 300.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra_lvd_ch1 = 1U, /**< PVD1 (m series, NMI-capable).      */
  k_ra_lvd_ch2 = 2U, /**< PVD2 (m series, NMI-capable).      */
  k_ra_lvd_ch4 = 4U, /**< PVD4 (n series, reset-only).       */
  k_ra_lvd_ch5 = 5U, /**< PVD5 (n series, reset-only).       */
} ra_lvd_channel_t;

/**
 * @struct ra_lvd_cfg_t
 * @brief Configuration descriptor for ``ra_lvd_channel_init``.
 *
 * @details
 * cppcheck cannot see tests/ so it flags struct fields as unused;
 * each member is consumed in ``ra_lvd_channel_init`` in
 * ``libs/ra_hal/src/ra_lvd.c``.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  ra_lvd_pvdlvl_t     threshold;    /**< PVDLVL[4:0] encoding (see enum).        */
  ra_lvd_edge_t       edge;         /**< IDTSEL setting (m channels only).       */
  ra_lvd_irq_type_t   irq_type;     /**< IRQSEL setting (m channels only).       */
  ra_lvd_response_t   response;     /**< Interrupt / reset / NMI / none.         */
  ra_lvd_negate_t     negate;       /**< PVDmCR0.RN (m chan, ignored on n).      */
  ra_lvd_hysteresis_t hysteresis;   /**< PVDmFCR.RHSEL / PVDnFCR.RHSEL.          */
  ra_lvd_loco_div_t   filter_div;   /**< FSAMP[1:0] sampling-clock divider.      */
  bool                filter_en;    /**< true -> clear DFDIS (digital filter on).*/
  bool                irq_enable;   /**< true -> set RIE on init (IRQ + reset).  */
  bool                clear_status; /**< true -> write 0 to PVDmSR.DET on init.  */
} ra_lvd_cfg_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @struct ra_lvd_status_t
 * @brief Decoded PVDmSR snapshot.
 *
 * @details
 * Only valid for monitor m channels (1, 2). Channels 4 and 5 do not
 * have a status register and ``ra_lvd_get_status`` returns
 * ``k_ra_err_not_supported`` for them.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  bool crossed; /**< DET flag: a Vdetm crossing was latched.        */
  bool above;   /**< MON flag: VCC currently above Vdetm threshold. */
} ra_lvd_status_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @typedef ra_lvd_event_fn_t
 * @brief PVD threshold-crossing callback.
 * @param[in] ctx     Caller context registered via ``ra_lvd_attach_handler``.
 * @param[in] channel Channel that fired (one of ``k_ra_lvd_chN``).
 */
typedef void (*ra_lvd_event_fn_t)(void* ctx, ra_lvd_channel_t channel);

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Configure one PVD channel from a full descriptor.
 *
 * @details
 * Runs the HUM Table 8.4 (m channel) or Table 8.6 (n channel) setup
 * sequence in full:
 *
 *   1. Disable the detector (PVDmCMPCR.PVDE = 0).
 *   2. Program PVDLVL[4:0] in PVDmCMPCR.
 *   3. Program PVDmFCR.RHSEL (m or n).
 *   4. Re-enable PVDE (the caller is responsible for the t_d(E-A)
 *      stabilisation delay before step 8 -- see `ra_lvd_filter_delay_us`).
 *   5. Program FSAMP[1:0] in PVDmCR0 with DFDIS still 1.
 *   6. Drop DFDIS to 0 (filter on) only if cfg->filter_en is true.
 *   7. Program PVDmCR1 (edge + IRQ type) -- m channels only.
 *   8. Set PVDmSR.DET = 0 (write-0-to-clear).
 *   9. Optionally set PVDmCR0.RIE / PVDnCR0.RE per cfg->irq_enable +
 *      cfg->response.
 *  10. Set PVDmCR0.CMPE = 1 to gate the comparator output through.
 *
 * @param[in] channel Channel id (k_ra_lvd_ch1, ch2, ch4, or ch5).
 * @param[in] cfg     Non-NULL configuration descriptor.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                Channel armed.
 * @retval k_ra_err_null_ptr      ``cfg`` was nullptr.
 * @retval k_ra_err_invalid_arg   Channel out of range, threshold outside
 *                                0x03..0x0F, edge = 0b11, filter_div > 3,
 *                                or RHSEL+RI conflict.
 * @retval k_ra_err_not_supported Response is interrupt/nmi on an n channel.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre PRCR.PRC3 has been unlocked by the caller.
 * @post On success, the chosen channel's PVDE and CMPE bits are set.
 * @post DET flag in PVDmSR is cleared (write-0-to-clear semantics).
 *
 * @par State Machine
 * @startuml
 * [*] --> Disabled : reset
 * Disabled --> Configured : channel_init
 * Configured --> Stabilising : PVDE = 1
 * Stabilising --> Active : t_d(E-A) elapsed + CMPE = 1
 * Active --> Disabled : channel_deinit
 * @enduml
 *
 * @note Thread safety: not thread-safe.
 * @see ra_lvd_channel_deinit
 * @since 0.11.0
 */
[[nodiscard]] ra_err_t ra_lvd_channel_init(ra_lvd_channel_t channel, const ra_lvd_cfg_t* cfg);

/**
 * @brief Tear one channel back down to its reset state.
 *
 * @details
 * Runs the HUM Table 8.5 (m) or Table 8.7 (n) shutdown sequence:
 *
 *   1. Clear PVDmCR0.CMPE.
 *   2. (Caller waits "2s + 3" LOCO cycles -- see `ra_lvd_filter_delay_us`.)
 *   3. Clear PVDmCR0.RIE / PVDnCR0.RE.
 *   4. Set PVDmCR0.DFDIS = 1 (filter off).
 *   5. Clear PVDmCMPCR.PVDE.
 *   6. Write 0 to PVDmSR.DET (m only) to clear any latched flag.
 *
 * @param[in] channel Channel id.
 * @return ``ra_err_t`` error code.
 *
 * @pre PRCR.PRC3 unlocked.
 * @pre ``channel`` is a valid id.
 * @post Channel's CMPCR / CR0 / CR1 / SR are zero.
 * @post Pending DET flag (if any) is cleared.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.11.0
 */
[[nodiscard]] ra_err_t ra_lvd_channel_deinit(ra_lvd_channel_t channel);

/* =============================================================================
 * Runtime reconfiguration
 * =============================================================================
 */

/**
 * @brief Change a channel's threshold without tearing the rest of its config.
 *
 * @details
 * Per HUM Ch 8.2.2 p 303 the PVDLVL field can only be changed while
 * **every** PVDmCMPCR.PVDE and PVDnCMPCR.PVDE bit is 0. This driver
 * temporarily clears the requested channel's own PVDE bit and writes
 * the new threshold; if other channels are running concurrently the
 * caller must serialise via ``ra_lvd_channel_deinit`` first.
 *
 * @param[in] channel   Channel id.
 * @param[in] threshold New PVDLVL encoding.
 *
 * @return ``ra_err_t`` error code.
 *
 * @pre PRCR.PRC3 unlocked.
 * @pre ``threshold`` falls in 0x03..0x0F.
 * @post Channel's PVDLVL[4:0] reflects the new value.
 * @post Channel's PVDE bit is restored to whatever value it held
 *       on entry.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.11.0
 */
[[nodiscard]] ra_err_t ra_lvd_set_threshold(ra_lvd_channel_t channel, ra_lvd_pvdlvl_t threshold);

/**
 * @brief Enable interrupt-on-cross for a monitor m channel.
 *
 * @details
 * Sets PVDmCR0.RIE = 1. The caller is responsible for the
 * stabilisation delay t_d(E-A) before this call (see HUM 8.2.2 p 303
 * "PVDE bit (Voltage Detection m Enable)").
 *
 * @param[in] channel Channel id (only k_ra_lvd_ch1 / ch2 are accepted;
 *                    n channels do not have RIE).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                Interrupt path armed.
 * @retval k_ra_err_invalid_arg   Channel id out of range.
 * @retval k_ra_err_not_supported Channel does not have an IRQ path.
 *
 * @pre PRCR.PRC3 unlocked.
 * @pre Caller has waited t_d(E-A) since enabling the detector.
 * @post PVDmCR0.RIE = 1 for the requested channel.
 * @post Subsequent threshold crossings will fire the registered
 *       callback (after `ra_lvd_dispatch` runs).
 *
 * @note Thread safety: not thread-safe.
 * @since 0.11.0
 */
[[nodiscard]] ra_err_t ra_lvd_enable_irq(ra_lvd_channel_t channel);

/**
 * @brief Drop PVDmCR0.RIE for one channel.
 *
 * @param[in] channel Channel id (m only).
 * @return ``ra_err_t`` error code.
 *
 * @pre PRCR.PRC3 unlocked.
 * @pre ``channel`` is one of {1, 2}.
 * @post PVDmCR0.RIE = 0; subsequent crossings won't fire IRQ/reset.
 * @post Comparator stays enabled -- DET / MON still update.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.11.0
 */
[[nodiscard]] ra_err_t ra_lvd_disable_irq(ra_lvd_channel_t channel);

/**
 * @brief Enable the reset path for a channel.
 *
 * @details
 * For m channels: sets RI = 1 (reset selected) and RIE = 1.
 * For n channels: sets RE = 1 (the only response option n has).
 *
 * @param[in] channel Channel id.
 * @return ``ra_err_t`` error code.
 *
 * @pre PRCR.PRC3 unlocked.
 * @pre Channel detector previously stabilised (t_d(E-A) elapsed).
 * @post Reset path armed; a qualifying crossing will trigger reset.
 * @post Deep Software Standby mode 2/3 is no longer reachable on m
 *       channels (HUM 8.2.4 p 305 RI bit note).
 *
 * @note Thread safety: not thread-safe.
 * @since 0.11.0
 */
[[nodiscard]] ra_err_t ra_lvd_enable_reset(ra_lvd_channel_t channel);

/**
 * @brief Drop the reset enable bit (RIE for m, RE for n).
 *
 * @param[in] channel Channel id.
 * @return ``ra_err_t`` error code.
 *
 * @pre PRCR.PRC3 unlocked.
 * @pre ``channel`` is one of {1,2,4,5}.
 * @post RIE/RE bit is zero.
 * @post On m channels RI is left untouched -- callers that need to
 *       reach Deep Software Standby 2/3 should also call
 *       `ra_lvd_cancel_deep_standby_path`.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.11.0
 */
[[nodiscard]] ra_err_t ra_lvd_disable_reset(ra_lvd_channel_t channel);

/**
 * @brief Drive PVDmCR0.CMPE / PVDnCR0.CMPE high for a channel.
 *
 * @details
 * Per HUM 8.2.4 p 305 "CMPE bit", CMPE must be set after the detector
 * has stabilised. This is the final step in Table 8.4 (step 13) and
 * Table 8.6 (step 10). Splitting CMPE off as its own API lets a caller
 * keep the detector and filter armed but throttle the downstream
 * comparator output during MRAM erase / programme windows (HUM 8.2.4
 * p 305 RIE bit warning).
 *
 * @param[in] channel Channel id.
 * @return ``ra_err_t`` error code.
 *
 * @pre PRCR.PRC3 unlocked.
 * @pre Detector enabled (PVDE = 1) and stabilisation time elapsed.
 * @post PVDmCR0.CMPE / PVDnCR0.CMPE = 1.
 * @post DET / MON flags become valid two PCLKB cycles later.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.11.0
 */
[[nodiscard]] ra_err_t ra_lvd_enable_cmpe(ra_lvd_channel_t channel);

/**
 * @brief Drop PVDmCR0.CMPE / PVDnCR0.CMPE for a channel.
 *
 * @param[in] channel Channel id.
 * @return ``ra_err_t`` error code.
 *
 * @pre PRCR.PRC3 unlocked.
 * @pre ``channel`` is one of {1,2,4,5}.
 * @post CMPE = 0; comparator output gated off.
 * @post DET / MON flags freeze at their last value (HUM 8.2.7 p 308).
 *
 * @note Thread safety: not thread-safe.
 * @since 0.11.0
 */
[[nodiscard]] ra_err_t ra_lvd_disable_cmpe(ra_lvd_channel_t channel);

/**
 * @brief Tune the digital filter (FSAMP[1:0] + DFDIS) on one channel.
 *
 * @details
 * Per HUM 8.2.4 p 305 "FSAMP[1:0] bits" the divider can be rewritten
 * only when DFDIS = 1. This routine enforces that:
 *
 *   1. DFDIS <- 1 (filter off).
 *   2. (Caller responsible for "2 LOCO cycles" wait per HUM Note 2 p 313.)
 *   3. FSAMP[1:0] <- new divider.
 *   4. DFDIS <- enable (per ``filter_en``).
 *
 * @param[in] channel    Channel id.
 * @param[in] filter_div New FSAMP[1:0] encoding (0..3).
 * @param[in] filter_en  true to leave the filter running, false to keep
 *                       it disabled.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok               Filter reconfigured.
 * @retval k_ra_err_invalid_arg  Channel or filter_div out of range.
 *
 * @pre PRCR.PRC3 unlocked.
 * @pre LOCOCR.LCSTP = 0 if ``filter_en`` is true (HUM 8.2.4 p 305).
 * @post FSAMP[1:0] reflects ``filter_div``.
 * @post DFDIS reflects !``filter_en``.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.11.0
 */
[[nodiscard]] ra_err_t
ra_lvd_set_filter(ra_lvd_channel_t channel, ra_lvd_loco_div_t filter_div, bool filter_en);

/**
 * @brief Update PVDmFCR.RHSEL / PVDnFCR.RHSEL hysteresis-band selector.
 *
 * @details
 * HUM 8.2.8 p 308 "RHSEL can be changed only if all the
 * PVDmCMPCR.PVDE bits and all the PVDnCMPCR.PVDE bits are 0." The
 * driver clears PVDE on the requested channel and restores it after
 * the write; if other channels are concurrently running the caller
 * must serialise.
 *
 * Setting RHSEL = 1 (HVD) when PVDmCR0.RI = 0 is forbidden (HUM
 * 8.2.8 p 308 last bullet); the driver returns ``k_ra_err_invalid_state``
 * in that case.
 *
 * @param[in] channel  Channel id.
 * @param[in] hyst     Hysteresis side selector.
 *
 * @return ``ra_err_t`` error code.
 *
 * @pre PRCR.PRC3 unlocked.
 * @pre ``hyst`` is one of {LVD, HVD}.
 * @post FCR.RHSEL reflects the requested value.
 * @post PVDE is restored to whatever value it had on entry.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.11.0
 */
[[nodiscard]] ra_err_t ra_lvd_set_hysteresis_mode(ra_lvd_channel_t    channel,
                                                  ra_lvd_hysteresis_t hyst);

/**
 * @brief Update PVDmCR0.RN reset-negate timing on an m channel.
 *
 * @details
 * HUM 8.2.4 p 305 "RN bit" describes four cases (RHSEL combined with
 * RN). The driver only enforces the universal rule "RN = 1 is
 * prohibited when RHSEL = 1" (HUM 8.2.4 p 306). The Software / Deep
 * Software Standby compatibility constraint ("the only possible value
 * for the RN bit is 0") is left to the caller.
 *
 * @param[in] channel  Channel id (m only).
 * @param[in] negate   New RN value.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                Updated.
 * @retval k_ra_err_invalid_arg   Channel out of range.
 * @retval k_ra_err_not_supported Channel is an n channel (no RN bit).
 * @retval k_ra_err_invalid_state RN = 1 attempted while RHSEL = 1.
 *
 * @pre PRCR.PRC3 unlocked.
 * @pre Channel is one of {1, 2}.
 * @post PVDmCR0.RN reflects the requested value.
 * @post Other CR0 fields are unchanged.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.11.0
 */
[[nodiscard]] ra_err_t ra_lvd_set_negate_mode(ra_lvd_channel_t channel, ra_lvd_negate_t negate);

/**
 * @brief Live update of PVDmCR1.IDTSEL[1:0] (edge select) for an m channel.
 *
 * @param[in] channel Channel id (m only).
 * @param[in] edge    New edge encoding.
 *
 * @return ``ra_err_t`` error code.
 *
 * @pre PRCR.PRC3 unlocked.
 * @pre ``edge`` != 0b11 (the prohibited setting per HUM 8.2.6 p 307).
 * @post PVDmCR1.IDTSEL reflects ``edge``.
 * @post IRQSEL is preserved.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.11.0
 */
[[nodiscard]] ra_err_t ra_lvd_set_irq_edge(ra_lvd_channel_t channel, ra_lvd_edge_t edge);

/**
 * @brief Live update of PVDmCR1.IRQSEL (NMI vs maskable) on an m channel.
 *
 * @param[in] channel Channel id (m only).
 * @param[in] kind    Maskable or NMI.
 *
 * @return ``ra_err_t`` error code.
 *
 * @pre PRCR.PRC3 unlocked.
 * @pre Channel is one of {1, 2}.
 * @post PVDmCR1.IRQSEL reflects ``kind``.
 * @post IDTSEL preserved.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.11.0
 */
[[nodiscard]] ra_err_t ra_lvd_set_irq_kind(ra_lvd_channel_t channel, ra_lvd_irq_type_t kind);

/* =============================================================================
 * Security attribution
 * =============================================================================
 */

/**
 * @brief Update PVDSAR.NONSEC0 / NONSEC1 to expose PVD1/PVD2 to non-secure.
 *
 * @details
 * HUM 8.2.1 p 302. The PVDSAR register is a single 32-bit cell with two
 * functional bits. Bits [31:2] are reserved (read-as-0 / write-0). PVD4
 * and PVD5 do not have a security-attribution bit and are always secure.
 *
 * @param[in] mask Bit mask formed from
 *                 ``k_ra_lvd_pvdsar_mask_nonsec0`` /
 *                 ``k_ra_lvd_pvdsar_mask_nonsec1``. Pass 0 to make both
 *                 channels secure-only.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok               Attribution updated.
 * @retval k_ra_err_invalid_arg  ``mask`` has any reserved bit set.
 *
 * @pre PRCR.PRC3 unlocked.
 * @pre Caller is currently in Secure world.
 * @post PVDSAR low two bits reflect ``mask``; reserved bits stay 0.
 * @post Subsequent non-secure aliasing of PVDmCMPCR / PVDmCR0 / PVDmCR1
 *       / PVDmSR succeeds for any channel whose NONSECx bit is now 1.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.11.0
 */
[[nodiscard]] ra_err_t ra_lvd_set_security(uint32_t mask);

/* =============================================================================
 * PVDLR (n-channel register lock)
 * =============================================================================
 */

/**
 * @brief Unlock PVD4/PVD5 control registers via PVDLR.
 *
 * @details
 * HUM 8.2.10 p 309. The PVDLR.LOCK bit starts at 1 after every POR /
 * RES-pin reset / PVD0 reset. While LOCK = 1, writes to PVD4CMPCR,
 * PVD5CMPCR, PVD4CR0, PVD5CR0, PVD4FCR, PVD5FCR are silently dropped.
 * Writing 0 once after a qualifying reset releases the lock; any
 * subsequent write to PVDLR latches LOCK back to 1 forever (until the
 * next qualifying reset).
 *
 * @return ``ra_err_t`` error code.
 *
 * @pre PRCR.PRC3 unlocked.
 * @pre This is the first PVDLR write since the last POR / RES / PVD0.
 * @post PVDLR.LOCK = 0; n-channel registers writable.
 * @post Re-locking is a one-shot: any subsequent ``relock`` is permanent.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.11.0
 */
[[nodiscard]] ra_err_t ra_lvd_unlock_n_channels(void);

/**
 * @brief Permanently re-lock PVD4/PVD5 by writing any value to PVDLR.
 *
 * @details
 * HUM 8.2.10 p 309 "After that, if you write an arbitrary value to the
 * LOCK, the LOCK bit is fixed to 1." Use this once n-channel
 * configuration is final. Calling ``unlock_n_channels`` again will not
 * re-open the registers until the next qualifying reset.
 *
 * @return ``ra_err_t`` error code.
 *
 * @pre PRCR.PRC3 unlocked.
 * @pre n-channel configuration is complete.
 * @post PVDLR.LOCK = 1; PVD4/PVD5 registers are write-protected.
 * @post No further unlock is possible until POR / RES / PVD0 reset.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.11.0
 */
[[nodiscard]] ra_err_t ra_lvd_relock_n_channels(void);

/* =============================================================================
 * Status
 * =============================================================================
 */

/**
 * @brief Read the DET and MON flags for one channel.
 *
 * @param[in]  channel Channel id (k_ra_lvd_ch1 or ch2 only).
 * @param[out] out     Decoded status (non-NULL).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                Status returned in ``*out``.
 * @retval k_ra_err_null_ptr      ``out`` was nullptr.
 * @retval k_ra_err_invalid_arg   Channel id out of range.
 * @retval k_ra_err_not_supported Channel has no status register.
 *
 * @pre ``out`` is non-NULL.
 * @pre ``channel`` is one of {1, 2}.
 * @post No hardware state is modified.
 * @post ``out->crossed`` reflects PVDmSR.DET; ``out->above`` reflects
 *       PVDmSR.MON.
 *
 * @note Thread safety: read-only, but caller should serialise with
 *       ``ra_lvd_clear_status`` to avoid losing a crossing.
 * @since 0.11.0
 */
[[nodiscard]] ra_err_t ra_lvd_get_status(ra_lvd_channel_t channel, ra_lvd_status_t* out);

/**
 * @brief Clear the latched DET flag on one channel (write-0-to-clear).
 *
 * @details
 * Per HUM 8.2.7 p 307 Note 1: only 0 can be written to DET. After the
 * write, two PCLKB cycles are required before DET reads as 0 again.
 * The driver does not spin -- callers that need to re-arm RIE
 * immediately should follow this call with the small busy-wait
 * documented in the HUM (or equivalent ``ra_time_delay_ns()`` helper
 * once it lands).
 *
 * @param[in] channel Channel id (k_ra_lvd_ch1 or ch2 only).
 *
 * @return ``ra_err_t`` error code.
 *
 * @pre PRCR.PRC3 unlocked.
 * @pre ``channel`` is one of {1, 2}.
 * @post PVDmSR.DET written to 0 (visible after 2 PCLKB cycles).
 * @post PVDmSR.MON is left untouched.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.11.0
 */
[[nodiscard]] ra_err_t ra_lvd_clear_status(ra_lvd_channel_t channel);

/* =============================================================================
 * ELC + standby helpers
 * =============================================================================
 */

/**
 * @brief Enable the ELC event-out path for an m channel.
 *
 * @details
 * Per HUM 8.7 p 315: the PVD has no dedicated ELC enable bit. The event
 * signal is asserted whenever the comparator's output is enabled
 * (PVDE = 1, CMPE = 1) AND the IRQ source fires (a Vdetm crossing per
 * IDTSEL). This routine therefore re-asserts CMPE and ensures DET is
 * cleared so the very first event after enabling does not get lost.
 *
 * The corresponding ELC event-source select belongs in `ra_elc` -- the
 * caller must call that path separately.
 *
 * @param[in] channel Channel id (m only -- HUM 8.7 p 315 limits ELC to PVDm).
 * @return ``ra_err_t`` error code.
 *
 * @pre PRCR.PRC3 unlocked.
 * @pre ``channel`` is one of {1, 2}.
 * @post CMPE = 1; DET = 0; the next crossing will assert the ELC event line.
 * @post No NVIC bits are touched (event path is independent of IRQ enable).
 *
 * @note Thread safety: not thread-safe.
 * @since 0.11.0
 */
[[nodiscard]] ra_err_t ra_lvd_enable_elc_event(ra_lvd_channel_t channel);

/**
 * @brief Disable the ELC event-out path for an m channel.
 *
 * @details
 * The HUM does not expose a dedicated bit. Per HUM 8.7 p 315 the
 * cleanest way to gate the event off is to clear CMPE; the comparator
 * output line goes "high" (driven inactive) and the ELC sees no edges.
 *
 * @param[in] channel Channel id (m only).
 * @return ``ra_err_t`` error code.
 *
 * @pre PRCR.PRC3 unlocked.
 * @pre Channel is one of {1, 2}.
 * @post CMPE = 0; subsequent crossings will not assert the ELC event line.
 * @post Detector remains powered (PVDE / RIE preserved).
 *
 * @note Thread safety: not thread-safe.
 * @since 0.11.0
 */
[[nodiscard]] ra_err_t ra_lvd_disable_elc_event(ra_lvd_channel_t channel);

/**
 * @brief Configure a channel for Software / Deep Software Standby use.
 *
 * @details
 * HUM 8.5(1) p 311 / HUM 8.5(2) p 312 require:
 *
 *   - DFDIS = 1 (digital filter must be off in standby because LOCO
 *     stops in some standby modes).
 *   - For Deep Software Standby: RI = 0 (otherwise standby modes 2 / 3
 *     become unreachable -- HUM 8.2.4 p 305 RI bit note).
 *
 * The routine performs both writes atomically (per channel) without
 * altering CMPE or PVDE.
 *
 * @param[in] channel Channel id.
 * @return ``ra_err_t`` error code.
 *
 * @pre PRCR.PRC3 unlocked.
 * @pre ``channel`` is one of {1,2,4,5}.
 * @post DFDIS = 1 on the requested channel.
 * @post On m channels, RI = 0 and RN = 0.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.11.0
 */
[[nodiscard]] ra_err_t ra_lvd_configure_for_standby(ra_lvd_channel_t channel);

/**
 * @brief Clear PVDmCR0.RI on every m channel so Deep Software Standby
 *        modes 2 and 3 are reachable.
 *
 * @details
 * HUM 8.2.4 p 305 "RI bit": "When the PVDmCR0.RI bit is 1 (voltage
 * monitor m reset selected), a transition to Deep Software Standby
 * mode 2 or 3 cannot be made". This convenience helper sweeps PVD1
 * and PVD2 in a single call so the LPM driver does not have to know
 * the PVD register layout.
 *
 * @return ``ra_err_t`` error code.
 *
 * @pre PRCR.PRC3 unlocked.
 * @pre Caller intends to enter Deep Software Standby 2 or 3.
 * @post PVD1CR0.RI = 0 and PVD2CR0.RI = 0.
 * @post All other CR0 fields preserved.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.11.0
 */
[[nodiscard]] ra_err_t ra_lvd_cancel_deep_standby_path(void);

/* =============================================================================
 * Filter timing helper
 * =============================================================================
 */

/**
 * @brief Compute the required "2s + 3 LOCO cycles" wait in microseconds.
 *
 * @details
 * Implements the HUM Table 8.4 step-8 / Table 8.6 step-8 wait formula:
 *
 *     us = (2 * 2^(div+1) + 3) * 1_000_000 / loco_hz + 1
 *
 * Pass ``loco_hz = k_ra_lvd_loco_hz_default`` for the nominal RA8D2
 * 32.768 kHz LOCO. The +1 us round-up matches FSP `r_lvd_filter_delay`.
 *
 * @param[in] div     FSAMP[1:0] encoding (0..3).
 * @param[in] loco_hz LOCO frequency (defaults expected -- pass 0 to use
 *                    `k_ra_lvd_loco_hz_default`).
 *
 * @return Required microseconds (>= 1).
 *
 * @pre ``div`` is in 0..3 (out-of-range silently clamps to 3).
 * @pre ``loco_hz`` is non-zero or 0 to take the default.
 * @post Return value is non-zero.
 * @post Function is pure -- no hardware access.
 *
 * @note Thread safety: pure function, MT-safe.
 * @since 0.11.0
 */
[[nodiscard]] uint32_t ra_lvd_filter_delay_us(ra_lvd_loco_div_t div, uint32_t loco_hz);

/* =============================================================================
 * Interrupt path
 * =============================================================================
 */

/**
 * @brief Attach a single shared callback for every PVD channel.
 *
 * @param[in] fn  Callback fired by ``ra_lvd_dispatch``.
 * @param[in] ctx Context forwarded to the callback.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Callback registered (or cleared if ``fn == nullptr``).
 *
 * @pre Caller is in single-threaded init context.
 * @pre ``fn`` may be nullptr (clears the callback).
 * @post Subsequent calls to ``ra_lvd_dispatch`` use the new callback.
 * @post Old callback is no longer invoked.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.11.0
 */
[[nodiscard]] ra_err_t ra_lvd_attach_handler(ra_lvd_event_fn_t fn, void* ctx);

/**
 * @brief Attach a per-channel callback (overrides the shared callback).
 *
 * @details
 * If the per-channel callback is registered for a channel, ``ra_lvd_dispatch``
 * fires it instead of the shared ``ra_lvd_attach_handler`` callback.
 * Pass ``fn = nullptr`` to clear the per-channel slot and fall back to
 * the shared callback.
 *
 * @param[in] channel Channel id (m only).
 * @param[in] fn      Per-channel callback or nullptr.
 * @param[in] ctx     Context for the callback.
 *
 * @return ``ra_err_t`` error code.
 *
 * @pre Single-threaded init context.
 * @pre ``channel`` is one of {1, 2}.
 * @post The per-channel slot for ``channel`` reflects ``fn`` / ``ctx``.
 * @post Shared callback is unchanged.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.11.0
 */
[[nodiscard]] ra_err_t
ra_lvd_attach_channel_handler(ra_lvd_channel_t channel, ra_lvd_event_fn_t fn, void* ctx);

/**
 * @brief Demux a PVD ISR -- invoke the registered callback for ``channel``.
 *
 * @details
 * Intended to be called from the secure-side ISR plumbing. Reads
 * PVDmSR.DET; if no crossing is latched the call is a no-op (so a
 * spurious IRQ does not invoke the callback). After the callback fires
 * and returns, DET is cleared via the write-0-to-clear sequence.
 *
 * @param[in] channel Channel id that fired the IRQ / NMI.
 *
 * @pre None.
 * @post If a callback is attached, it has been invoked exactly once.
 * @post PVDmSR.DET is 0 on return.
 *
 * @note Thread safety: ISR-context only.
 * @since 0.11.0
 */
void ra_lvd_dispatch(ra_lvd_channel_t channel);

#ifdef __cplusplus
}
#endif
