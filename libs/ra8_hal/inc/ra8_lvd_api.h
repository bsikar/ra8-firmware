/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_lvd_api.h
 * @brief Programmable Voltage Detection (PVD / LVD) HAL function prototypes
 * @ingroup grp_hal_system
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Public function prototypes for the RA8D2 PVD HAL driver: lifecycle,
 * runtime reconfiguration, security attribution, n-channel register
 * lock, status, ELC + standby helpers, the filter-timing helper, and
 * the interrupt path. Split out of ra8_lvd.h so the public umbrella
 * header stays under the per-file line budget; the umbrella ra8_lvd.h
 * re-includes this header, so consumers are unaffected.
 *
 * See ra8_lvd.h for the driver overview and HUM Ch 8 "Programmable
 * Voltage Detection (PVD)", p 300-316 (R01UH1065EJ rev 1.30).
 *
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_lvd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

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
 *      stabilisation delay before step 8 -- see `ra8_lvd_filter_delay_us`).
 *   5. Program FSAMP[1:0] in PVDmCR0 with DFDIS still 1.
 *   6. Drop DFDIS to 0 (filter on) only if cfg->filter_en is true.
 *   7. Program PVDmCR1 (edge + IRQ type) -- m channels only.
 *   8. Set PVDmSR.DET = 0 (write-0-to-clear).
 *   9. Optionally set PVDmCR0.RIE / PVDnCR0.RE per cfg->irq_enable +
 *      cfg->response.
 *  10. Set PVDmCR0.CMPE = 1 to gate the comparator output through.
 *
 * @param[in] channel Channel id (k_ra8_lvd_ch1, ch2, ch4, or ch5).
 * @param[in] cfg     Non-NULL configuration descriptor.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Channel armed.
 * @retval k_ra8_err_null_ptr      ``cfg`` was nullptr.
 * @retval k_ra8_err_invalid_arg   Channel out of range, threshold outside
 *                                0x03..0x0F, edge = 0b11, filter_div > 3,
 *                                or RHSEL+RI conflict.
 * @retval k_ra8_err_not_supported Response is interrupt/nmi on an n channel.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre PRCR.PRC3 has been unlocked by the caller.
 * @post On success, the chosen channel's PVDE and CMPE bits are set.
 * @post DET flag in PVDmSR is cleared (write-0-to-clear semantics).
 *
 * @par State Machine
 * @dot
 * digraph ra8_lvd_api_states {
 *   bgcolor="transparent";
 *   rankdir=LR;
 *   node [shape=box, style="rounded,filled", fontname="Helvetica", fontsize=10,
 *         fillcolor="#e8eef7", color="#5a7ca6"];
 *   edge [fontname="Helvetica", fontsize=9, color="#5a7ca6"];
 *
 *   __start [shape=circle, width=0.18, label="", fillcolor="#5a7ca6", color="#5a7ca6"];
 *
 *   Disabled [label="Disabled"];
 *   Configured [label="Configured"];
 *   Stabilising [label="Stabilising"];
 *   Active [label="Active"];
 *
 *   __start -> Disabled [label="reset"];
 *   Disabled -> Configured [label="channel_init"];
 *   Configured -> Stabilising [label="PVDE = 1"];
 *   Stabilising -> Active [label="t_d(E-A) elapsed + CMPE = 1"];
 *   Active -> Disabled [label="channel_deinit"];
 * }
 * @enddot
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_lvd_channel_deinit
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lvd_channel_init(ra8_lvd_channel_t channel, const ra8_lvd_cfg_t* cfg);

/**
 * @brief Tear one channel back down to its reset state.
 *
 * @details
 * Runs the HUM Table 8.5 (m) or Table 8.7 (n) shutdown sequence:
 *
 *   1. Clear PVDmCR0.CMPE.
 *   2. (Caller waits "2s + 3" LOCO cycles -- see `ra8_lvd_filter_delay_us`.)
 *   3. Clear PVDmCR0.RIE / PVDnCR0.RE.
 *   4. Set PVDmCR0.DFDIS = 1 (filter off).
 *   5. Clear PVDmCMPCR.PVDE.
 *   6. Write 0 to PVDmSR.DET (m only) to clear any latched flag.
 *
 * @param[in] channel Channel id.
 * @return ``ra8_err_t`` error code.
 *
 * @pre PRCR.PRC3 unlocked.
 * @pre ``channel`` is a valid id.
 * @post Channel's CMPCR / CR0 / CR1 / SR are zero.
 * @post Pending DET flag (if any) is cleared.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lvd_channel_deinit(ra8_lvd_channel_t channel);

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
 * caller must serialise via ``ra8_lvd_channel_deinit`` first.
 *
 * @param[in] channel   Channel id.
 * @param[in] threshold New PVDLVL encoding.
 *
 * @return ``ra8_err_t`` error code.
 *
 * @pre PRCR.PRC3 unlocked.
 * @pre ``threshold`` falls in 0x03..0x0F.
 * @post Channel's PVDLVL[4:0] reflects the new value.
 * @post Channel's PVDE bit is restored to whatever value it held
 *       on entry.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lvd_set_threshold(ra8_lvd_channel_t channel,
                                              ra8_lvd_pvdlvl_t  threshold);

/**
 * @brief Enable interrupt-on-cross for a monitor m channel.
 *
 * @details
 * Sets PVDmCR0.RIE = 1. The caller is responsible for the
 * stabilisation delay t_d(E-A) before this call (see HUM 8.2.2 p 303
 * "PVDE bit (Voltage Detection m Enable)").
 *
 * @param[in] channel Channel id (only k_ra8_lvd_ch1 / ch2 are accepted;
 *                    n channels do not have RIE).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Interrupt path armed.
 * @retval k_ra8_err_invalid_arg   Channel id out of range.
 * @retval k_ra8_err_not_supported Channel does not have an IRQ path.
 *
 * @pre PRCR.PRC3 unlocked.
 * @pre Caller has waited t_d(E-A) since enabling the detector.
 * @post PVDmCR0.RIE = 1 for the requested channel.
 * @post Subsequent threshold crossings will fire the registered
 *       callback (after `ra8_lvd_dispatch` runs).
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lvd_enable_irq(ra8_lvd_channel_t channel);

/**
 * @brief Drop PVDmCR0.RIE for one channel.
 *
 * @param[in] channel Channel id (m only).
 * @return ``ra8_err_t`` error code.
 *
 * @pre PRCR.PRC3 unlocked.
 * @pre ``channel`` is one of {1, 2}.
 * @post PVDmCR0.RIE = 0; subsequent crossings won't fire IRQ/reset.
 * @post Comparator stays enabled -- DET / MON still update.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lvd_disable_irq(ra8_lvd_channel_t channel);

/**
 * @brief Enable the reset path for a channel.
 *
 * @details
 * For m channels: sets RI = 1 (reset selected) and RIE = 1.
 * For n channels: sets RE = 1 (the only response option n has).
 *
 * @param[in] channel Channel id.
 * @return ``ra8_err_t`` error code.
 *
 * @pre PRCR.PRC3 unlocked.
 * @pre Channel detector previously stabilised (t_d(E-A) elapsed).
 * @post Reset path armed; a qualifying crossing will trigger reset.
 * @post Deep Software Standby mode 2/3 is no longer reachable on m
 *       channels (HUM 8.2.4 p 305 RI bit note).
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lvd_enable_reset(ra8_lvd_channel_t channel);

/**
 * @brief Drop the reset enable bit (RIE for m, RE for n).
 *
 * @param[in] channel Channel id.
 * @return ``ra8_err_t`` error code.
 *
 * @pre PRCR.PRC3 unlocked.
 * @pre ``channel`` is one of {1,2,4,5}.
 * @post RIE/RE bit is zero.
 * @post On m channels RI is left untouched -- callers that need to
 *       reach Deep Software Standby 2/3 should also call
 *       `ra8_lvd_cancel_deep_standby_path`.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lvd_disable_reset(ra8_lvd_channel_t channel);

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
 * @return ``ra8_err_t`` error code.
 *
 * @pre PRCR.PRC3 unlocked.
 * @pre Detector enabled (PVDE = 1) and stabilisation time elapsed.
 * @post PVDmCR0.CMPE / PVDnCR0.CMPE = 1.
 * @post DET / MON flags become valid two PCLKB cycles later.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lvd_enable_cmpe(ra8_lvd_channel_t channel);

/**
 * @brief Drop PVDmCR0.CMPE / PVDnCR0.CMPE for a channel.
 *
 * @param[in] channel Channel id.
 * @return ``ra8_err_t`` error code.
 *
 * @pre PRCR.PRC3 unlocked.
 * @pre ``channel`` is one of {1,2,4,5}.
 * @post CMPE = 0; comparator output gated off.
 * @post DET / MON flags freeze at their last value (HUM 8.2.7 p 308).
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lvd_disable_cmpe(ra8_lvd_channel_t channel);

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
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               Filter reconfigured.
 * @retval k_ra8_err_invalid_arg  Channel or filter_div out of range.
 *
 * @pre PRCR.PRC3 unlocked.
 * @pre LOCOCR.LCSTP = 0 if ``filter_en`` is true (HUM 8.2.4 p 305).
 * @post FSAMP[1:0] reflects ``filter_div``.
 * @post DFDIS reflects !``filter_en``.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_lvd_set_filter(ra8_lvd_channel_t channel, ra8_lvd_loco_div_t filter_div, bool filter_en);

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
 * 8.2.8 p 308 last bullet); the driver returns ``k_ra8_err_invalid_state``
 * in that case.
 *
 * @param[in] channel  Channel id.
 * @param[in] hyst     Hysteresis side selector.
 *
 * @return ``ra8_err_t`` error code.
 *
 * @pre PRCR.PRC3 unlocked.
 * @pre ``hyst`` is one of {LVD, HVD}.
 * @post FCR.RHSEL reflects the requested value.
 * @post PVDE is restored to whatever value it had on entry.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lvd_set_hysteresis_mode(ra8_lvd_channel_t    channel,
                                                    ra8_lvd_hysteresis_t hyst);

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
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Updated.
 * @retval k_ra8_err_invalid_arg   Channel out of range.
 * @retval k_ra8_err_not_supported Channel is an n channel (no RN bit).
 * @retval k_ra8_err_invalid_state RN = 1 attempted while RHSEL = 1.
 *
 * @pre PRCR.PRC3 unlocked.
 * @pre Channel is one of {1, 2}.
 * @post PVDmCR0.RN reflects the requested value.
 * @post Other CR0 fields are unchanged.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lvd_set_negate_mode(ra8_lvd_channel_t channel, ra8_lvd_negate_t negate);

/**
 * @brief Live update of PVDmCR1.IDTSEL[1:0] (edge select) for an m channel.
 *
 * @param[in] channel Channel id (m only).
 * @param[in] edge    New edge encoding.
 *
 * @return ``ra8_err_t`` error code.
 *
 * @pre PRCR.PRC3 unlocked.
 * @pre ``edge`` != 0b11 (the prohibited setting per HUM 8.2.6 p 307).
 * @post PVDmCR1.IDTSEL reflects ``edge``.
 * @post IRQSEL is preserved.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lvd_set_irq_edge(ra8_lvd_channel_t channel, ra8_lvd_edge_t edge);

/**
 * @brief Live update of PVDmCR1.IRQSEL (NMI vs maskable) on an m channel.
 *
 * @param[in] channel Channel id (m only).
 * @param[in] kind    Maskable or NMI.
 *
 * @return ``ra8_err_t`` error code.
 *
 * @pre PRCR.PRC3 unlocked.
 * @pre Channel is one of {1, 2}.
 * @post PVDmCR1.IRQSEL reflects ``kind``.
 * @post IDTSEL preserved.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lvd_set_irq_kind(ra8_lvd_channel_t channel, ra8_lvd_irq_type_t kind);

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
 *                 ``k_ra8_lvd_pvdsar_mask_nonsec0`` /
 *                 ``k_ra8_lvd_pvdsar_mask_nonsec1``. Pass 0 to make both
 *                 channels secure-only.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               Attribution updated.
 * @retval k_ra8_err_invalid_arg  ``mask`` has any reserved bit set.
 *
 * @pre PRCR.PRC3 unlocked.
 * @pre Caller is currently in Secure world.
 * @post PVDSAR low two bits reflect ``mask``; reserved bits stay 0.
 * @post Subsequent non-secure aliasing of PVDmCMPCR / PVDmCR0 / PVDmCR1
 *       / PVDmSR succeeds for any channel whose NONSECx bit is now 1.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lvd_set_security(uint32_t mask);

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
 * @return ``ra8_err_t`` error code.
 *
 * @pre PRCR.PRC3 unlocked.
 * @pre This is the first PVDLR write since the last POR / RES / PVD0.
 * @post PVDLR.LOCK = 0; n-channel registers writable.
 * @post Re-locking is a one-shot: any subsequent ``relock`` is permanent.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lvd_unlock_n_channels(void);

/**
 * @brief Permanently re-lock PVD4/PVD5 by writing any value to PVDLR.
 *
 * @details
 * HUM 8.2.10 p 309 "After that, if you write an arbitrary value to the
 * LOCK, the LOCK bit is fixed to 1." Use this once n-channel
 * configuration is final. Calling ``unlock_n_channels`` again will not
 * re-open the registers until the next qualifying reset.
 *
 * @return ``ra8_err_t`` error code.
 *
 * @pre PRCR.PRC3 unlocked.
 * @pre n-channel configuration is complete.
 * @post PVDLR.LOCK = 1; PVD4/PVD5 registers are write-protected.
 * @post No further unlock is possible until POR / RES / PVD0 reset.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lvd_relock_n_channels(void);

/* =============================================================================
 * Status
 * =============================================================================
 */

/**
 * @brief Read the DET and MON flags for one channel.
 *
 * @param[in]  channel Channel id (k_ra8_lvd_ch1 or ch2 only).
 * @param[out] out     Decoded status (non-NULL).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Status returned in ``*out``.
 * @retval k_ra8_err_null_ptr      ``out`` was nullptr.
 * @retval k_ra8_err_invalid_arg   Channel id out of range.
 * @retval k_ra8_err_not_supported Channel has no status register.
 *
 * @pre ``out`` is non-NULL.
 * @pre ``channel`` is one of {1, 2}.
 * @post No hardware state is modified.
 * @post ``out->crossed`` reflects PVDmSR.DET; ``out->above`` reflects
 *       PVDmSR.MON.
 *
 * @note Thread safety: read-only, but caller should serialise with
 *       ``ra8_lvd_clear_status`` to avoid losing a crossing.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lvd_get_status(ra8_lvd_channel_t channel, ra8_lvd_status_t* out);

/**
 * @brief Clear the latched DET flag on one channel (write-0-to-clear).
 *
 * @details
 * Per HUM 8.2.7 p 307 Note 1: only 0 can be written to DET. After the
 * write, two PCLKB cycles are required before DET reads as 0 again.
 * The driver does not spin -- callers that need to re-arm RIE
 * immediately should follow this call with the small busy-wait
 * documented in the HUM (or equivalent ``ra8_time_delay_ns()`` helper
 * once it lands).
 *
 * @param[in] channel Channel id (k_ra8_lvd_ch1 or ch2 only).
 *
 * @return ``ra8_err_t`` error code.
 *
 * @pre PRCR.PRC3 unlocked.
 * @pre ``channel`` is one of {1, 2}.
 * @post PVDmSR.DET written to 0 (visible after 2 PCLKB cycles).
 * @post PVDmSR.MON is left untouched.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lvd_clear_status(ra8_lvd_channel_t channel);

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
 * The corresponding ELC event-source select belongs in `ra8_elc` -- the
 * caller must call that path separately.
 *
 * @param[in] channel Channel id (m only -- HUM 8.7 p 315 limits ELC to PVDm).
 * @return ``ra8_err_t`` error code.
 *
 * @pre PRCR.PRC3 unlocked.
 * @pre ``channel`` is one of {1, 2}.
 * @post CMPE = 1; DET = 0; the next crossing will assert the ELC event line.
 * @post No NVIC bits are touched (event path is independent of IRQ enable).
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lvd_enable_elc_event(ra8_lvd_channel_t channel);

/**
 * @brief Disable the ELC event-out path for an m channel.
 *
 * @details
 * The HUM does not expose a dedicated bit. Per HUM 8.7 p 315 the
 * cleanest way to gate the event off is to clear CMPE; the comparator
 * output line goes "high" (driven inactive) and the ELC sees no edges.
 *
 * @param[in] channel Channel id (m only).
 * @return ``ra8_err_t`` error code.
 *
 * @pre PRCR.PRC3 unlocked.
 * @pre Channel is one of {1, 2}.
 * @post CMPE = 0; subsequent crossings will not assert the ELC event line.
 * @post Detector remains powered (PVDE / RIE preserved).
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lvd_disable_elc_event(ra8_lvd_channel_t channel);

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
 * @return ``ra8_err_t`` error code.
 *
 * @pre PRCR.PRC3 unlocked.
 * @pre ``channel`` is one of {1,2,4,5}.
 * @post DFDIS = 1 on the requested channel.
 * @post On m channels, RI = 0 and RN = 0.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lvd_configure_for_standby(ra8_lvd_channel_t channel);

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
 * @return ``ra8_err_t`` error code.
 *
 * @pre PRCR.PRC3 unlocked.
 * @pre Caller intends to enter Deep Software Standby 2 or 3.
 * @post PVD1CR0.RI = 0 and PVD2CR0.RI = 0.
 * @post All other CR0 fields preserved.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lvd_cancel_deep_standby_path(void);

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
 * Pass ``loco_hz = k_ra8_lvd_loco_hz_default`` for the nominal RA8D2
 * 32.768 kHz LOCO. The +1 us round-up matches FSP `r_lvd_filter_delay`.
 *
 * @param[in] div     FSAMP[1:0] encoding (0..3).
 * @param[in] loco_hz LOCO frequency (defaults expected -- pass 0 to use
 *                    `k_ra8_lvd_loco_hz_default`).
 *
 * @return Required microseconds (>= 1).
 *
 * @pre ``div`` is in 0..3 (out-of-range silently clamps to 3).
 * @pre ``loco_hz`` is non-zero or 0 to take the default.
 * @post Return value is non-zero.
 * @post Function is pure -- no hardware access.
 *
 * @note Thread safety: pure function, MT-safe.
 * @since 0.1.0
 */
[[nodiscard]] uint32_t ra8_lvd_filter_delay_us(ra8_lvd_loco_div_t div, uint32_t loco_hz);

/* =============================================================================
 * Interrupt path
 * =============================================================================
 */

/**
 * @brief Attach a single shared callback for every PVD channel.
 *
 * @param[in] fn  Callback fired by ``ra8_lvd_dispatch``.
 * @param[in] ctx Context forwarded to the callback.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Callback registered (or cleared if ``fn == nullptr``).
 *
 * @pre Caller is in single-threaded init context.
 * @pre ``fn`` may be nullptr (clears the callback).
 * @post Subsequent calls to ``ra8_lvd_dispatch`` use the new callback.
 * @post Old callback is no longer invoked.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lvd_attach_handler(ra8_lvd_event_fn_t fn, void* ctx);

/**
 * @brief Attach a per-channel callback (overrides the shared callback).
 *
 * @details
 * If the per-channel callback is registered for a channel, ``ra8_lvd_dispatch``
 * fires it instead of the shared ``ra8_lvd_attach_handler`` callback.
 * Pass ``fn = nullptr`` to clear the per-channel slot and fall back to
 * the shared callback.
 *
 * @param[in] channel Channel id (m only).
 * @param[in] fn      Per-channel callback or nullptr.
 * @param[in] ctx     Context for the callback.
 *
 * @return ``ra8_err_t`` error code.
 *
 * @pre Single-threaded init context.
 * @pre ``channel`` is one of {1, 2}.
 * @post The per-channel slot for ``channel`` reflects ``fn`` / ``ctx``.
 * @post Shared callback is unchanged.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_lvd_attach_channel_handler(ra8_lvd_channel_t channel, ra8_lvd_event_fn_t fn, void* ctx);

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
 * @pre Called from PVD ISR or NMI context.
 * @pre ``channel`` is a valid ::ra8_lvd_channel_t value.
 *
 * @post If a callback is attached, it has been invoked exactly once.
 * @post PVDmSR.DET is 0 on return.
 *
 * @note Thread safety: ISR-context only. See HUM Ch 12 "Low Voltage
 *       Detection (LVD)" pp 593-624 for register semantics.
 * @since 0.1.0
 */
void ra8_lvd_dispatch(ra8_lvd_channel_t channel);

#ifdef __cplusplus
}
#endif
