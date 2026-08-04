/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_canfd.h
 * @brief CANFD Lite driver (bit-timing, TX, RX, error state)
 * @ingroup grp_hal_comms
 *
 * @details
 * Minimal driver surface over the RA8D2 CANFD Lite controller --
 * enough to bring a channel up at a chosen nominal bit rate, queue a
 * CAN 2.0B / CAN-FD frame into the TX message buffer, poll the RX
 * FIFO for inbound frames, and read the TX/RX error counters.
 *
 * DMA hooks, interrupt delivery, and the full acceptance-filter bank
 * are deferred until there is a real consumer that needs them.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_canfd_regs.h"
#include "ra8_err.h"

/**
 * @struct ra8_canfd_frame_t
 * @brief A single CAN / CAN-FD frame as seen by the driver public API.
 *
 * @details
 * `id` holds an 11-bit standard or 29-bit extended identifier (masked
 * to `k_ra8_canfd_id_std_mask` / `k_ra8_canfd_id_ext_mask` on send).
 * `dlc` is the raw DLC nibble (0..15) -- callers passing a length
 * longer than 8 bytes must also set `is_fd`.
 *
 * cppcheck cannot see tests/ so it flags every field as unused;
 * each member is read in ``ra8_canfd_transmit`` and
 * ``ra8_canfd_receive`` in ``libs/ra8_hal/src/ra8_canfd.c``.
 *
 * @invariant `dlc <= 15`
 * @invariant If `is_extended == 0`, `id` fits in 11 bits.
 * @invariant If `is_brs`, the channel must have been initialized with
 *            a data bit rate > nominal bit rate.
 *
 * @note The `data` array is sized for the worst case (64-byte CAN-FD
 *       payload). Classic CAN frames only touch the first 8 bytes.
 */
typedef struct {
  uint32_t id;                               /**< Arbitration-ID field.                   */
  uint8_t  dlc;                              /**< DLC code (0..15). See CAN-FD DLC table. */
  uint8_t  is_extended;                      /**< 0 = 11-bit ID, 1 = 29-bit ID.           */
  uint8_t  is_fd;                            /**< 0 = classic CAN, 1 = CAN-FD.            */
  uint8_t  is_brs;                           /**< 1 = bit-rate switch in data phase.      */
  uint8_t  data[k_ra8_canfd_data_bytes_max]; /**< Payload bytes.                          */
} ra8_canfd_frame_t;

/**
 * @brief Take a CANFD channel out of reset into operation mode.
 *
 * @param[in] channel Channel index (0..1).
 * @return `k_ra8_ok` on success, `k_ra8_err_null_ptr` if `channel` out of range.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_canfd_init(uint8_t channel);

/**
 * @brief Put the CANFD channel back into reset.
 *
 * @param[in] channel Channel index (0..1).
 * @return `k_ra8_ok` on success, `k_ra8_err_null_ptr` if `channel` out of range.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_canfd_deinit(uint8_t channel);

/**
 * @brief Programme nominal (and optional data) bit-timing registers.
 *
 * @details
 * Uses PCLKA as the CAN source clock (HUM default). Targets a 75%
 * sample point (TSEG1=15, TSEG2=4, SJW=min(4,TSEG2)) and rejects any
 * bit rate that does not resolve to an integer prescaler in 1..256.
 *
 * @param[in] channel          Channel index (0..1).
 * @param[in] bitrate_bps      Nominal-phase bit rate in bits per second.
 * @param[in] data_bitrate_bps Data-phase bit rate (for CAN-FD). Set to
 *                             `bitrate_bps` (or 0) for classic-only.
 *
 * @return `k_ra8_err_null_ptr` if `channel` out of range.
 * @return `k_ra8_err_invalid_arg` if either bit rate is zero or does
 *         not resolve to a valid prescaler / timing triple.
 * @return `k_ra8_ok` on success.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_canfd_set_bitrate(uint8_t channel, uint32_t bitrate_bps, uint32_t data_bitrate_bps);

/**
 * @brief Queue a frame into the TX message buffer and trigger transmission.
 *
 * @details
 * Fire-and-forget: the routine validates every field, writes the ID /
 * DLC / FD-status / payload into the TX message-buffer registers,
 * then asserts `TMTR` in `CFDTMC`. No completion callback is raised.
 *
 * @param[in] channel Channel index (0..1).
 * @param[in] frame   Frame descriptor. Must not be NULL.
 *
 * @return `k_ra8_err_null_ptr`   if `channel` out of range or `frame == NULL`.
 * @return `k_ra8_err_invalid_arg` on bad DLC, oversized ID, or inconsistent flags.
 * @return `k_ra8_ok` on success.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_canfd_transmit(uint8_t channel, const ra8_canfd_frame_t* frame);

/**
 * @brief Poll the RX FIFO for the next available frame.
 *
 * @details
 * Non-blocking: on an empty FIFO the call returns `k_ra8_err_no_data`
 * immediately. On success the frame is popped (by writing the pointer
 * control register) before returning.
 *
 * @param[in]  channel   Channel index (0..1).
 * @param[out] out_frame Destination frame. Must not be NULL.
 *
 * @return `k_ra8_err_null_ptr`  if `channel` out of range or `out_frame == NULL`.
 * @return `k_ra8_err_no_data`   if the RX FIFO is empty.
 * @return `k_ra8_ok`            on success.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_canfd_receive(uint8_t channel, ra8_canfd_frame_t* out_frame);

/**
 * @brief Read the TX and RX error counters.
 *
 * @param[in]  channel Channel index (0..1).
 * @param[out] tx_err  TEC counter value. Must not be NULL.
 * @param[out] rx_err  REC counter value. Must not be NULL.
 *
 * @return `k_ra8_err_null_ptr` if any pointer is NULL or channel out of range.
 * @return `k_ra8_ok`           on success.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_canfd_get_error_state(uint8_t channel, uint8_t* tx_err, uint8_t* rx_err);

/**
 * @typedef ra8_canfd_event_fn_t
 * @brief CANFD event callback.
 */
typedef void (*ra8_canfd_event_fn_t)(void* ctx, uint8_t channel, uint32_t status_mask);

/**
 * @brief Read the channel status register (CFDCnSTS).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_canfd_get_status(uint8_t channel, uint32_t* out_mask);

/**
 * @brief Clear error flags in CFDCnERFL.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_canfd_clear_status(uint8_t channel, uint32_t mask);

/**
 * @brief Attach a callback for CANFD events (shared across channels).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_canfd_attach_handler(ra8_canfd_event_fn_t fn, void* ctx);

/**
 * @brief Dispatch a CANFD event for a channel -- snapshot + fire callback.
 * @since 0.1.0
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @param[in] channel See header declaration for direction and constraints.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 */
void ra8_canfd_dispatch(uint8_t channel);

/**
 * @brief Programme one Global Acceptance-Filter-List (GAFL) entry.
 *
 * @details
 * Selects the AFL page that contains @p filter_id (each page holds
 * 16 entries) by writing CFDGAFLECTR.AFLPN with the page index and
 * unlocking the data window with CFDGAFLECTR.AFLDAE.  Then writes
 * CFDGAFL[idx].ID with the accept ID, CFDGAFL[idx].M with the bit
 * mask, and CFDGAFL[idx].P1 with the DLC field used to gate fast
 * RX-FIFO routing.  See HUM Ch 41 "CFDGAFLECTR / CFDGAFL"
 * pp 2702-2867.
 *
 * @param[in] filter_id  Filter index (0..k_ra8_canfd_afl_total - 1).
 * @param[in] accept_id  Raw arbitration ID to accept (11- or 29-bit).
 * @param[in] mask       Bit mask -- bits clear are "don't care".
 * @param[in] dlc        DLC code packed into CFDGAFL.P1 (0..15).
 *
 * @return ::ra8_err_t outcome.
 * @retval k_ra8_ok               Filter slot programmed.
 * @retval k_ra8_err_invalid_arg  @p filter_id, @p accept_id, or @p dlc out
 *                               of range.
 *
 * @pre  Channel is initialized; controller is in global-reset mode.
 * @pre  Caller is single-threaded init context.
 * @post CFDGAFL[entry].ID/M/P1 reflect the requested rule.
 * @post CFDGAFLECTR.AFLDAE re-locked when the call returns.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_canfd_filter_set(uint16_t filter_id, uint32_t accept_id, uint32_t mask, uint8_t dlc);

/**
 * @enum ra8_canfd_afl_capacity_t
 * @brief Per-channel Acceptance-Filter-List capacity used by
 *        ::ra8_canfd_set_accept_filter.
 *
 * @details
 * ::ra8_canfd_set_accept_filter installs its rules into a single 16-entry
 * AFL page (page 0). The CFDGAFLECTR.AFLPN page selector is 4 bits and
 * each page exposes ::k_ra8_canfd_afl_page_size entries; one page is the
 * working set the list-config API supports, so the rule count is capped
 * here. HUM Ch 41.2.18 "CFDGAFLCFG0 : Global AFL Configuration
 * Register 0" p 2735 (RNC field) and Ch 41.2.17 "CFDGAFLECTR" p 2734.
 */
typedef enum : uint8_t {
  k_ra8_canfd_afl_rule_capacity = 16U, /**< One 16-entry AFL page (page 0). */
} ra8_canfd_afl_capacity_t;

/**
 * @struct ra8_canfd_afl_rule_t
 * @brief One Acceptance-Filter-List (AFL) rule: hardware ID/mask match
 *        plus an RX-FIFO routing target.
 *
 * @details
 * Each rule programs one CFDGAFL slot. The controller accepts an inbound
 * frame when, for some active rule, ``(frame_id & mask) == (id & mask)``
 * and the frame's IDE flag equals @p extended and its RTR flag equals
 * @p rtr; matched frames are routed into RX FIFO @p target_rx instead of
 * being dropped. @p id and @p mask are right-aligned identifiers: an
 * 11-bit standard value when @p extended is false, a 29-bit extended
 * value when true.
 *
 * cppcheck cannot see tests/ so it flags every field as unused; each
 * member is read in ``ra8_canfd_set_accept_filter`` in
 * ``libs/ra8_hal/src/ra8_canfd_afl.c``.
 *
 * @invariant If @p extended is false, @p id and @p mask fit in 11 bits.
 * @invariant If @p extended is true, @p id and @p mask fit in 29 bits.
 * @invariant @p target_rx < ::k_ra8_canfd_rx_fifo_count.
 *
 * @see ra8_canfd_set_accept_filter()
 */
typedef struct {
  uint32_t id;        /**< Right-aligned accept ID (11- or 29-bit).         */
  uint32_t mask;      /**< ID bit mask: 1 = must match, 0 = don't care.     */
  bool     extended;  /**< true = 29-bit extended ID, false = 11-bit std.   */
  bool     rtr;       /**< true = match remote frames, false = data frames. */
  uint8_t  target_rx; /**< Destination RX FIFO index (0..1).                */
} ra8_canfd_afl_rule_t;

/**
 * @brief Program the channel Acceptance-Filter-List from a rule array.
 *
 * @details
 * Installs @p count hardware acceptance rules into AFL page 0 for
 * @p channel, replacing the single pass-all rule that ::ra8_canfd_init
 * installs. The routine follows the documented AFL edit transaction
 * exactly:
 *   1. Unlock the AFL data window and select page 0 by writing
 *      CFDGAFLECTR.AFLPN + CFDGAFLECTR.AFLDAE.
 *   2. Set the page-0 rule count in CFDGAFLCFG0.RNC0.
 *   3. Write each rule's CFDGAFLID / CFDGAFLM / CFDGAFLP0 / CFDGAFLP1.
 *   4. Re-lock the window by clearing CFDGAFLECTR.
 *
 * For a rule, IDE and RTR are added to the match (CFDGAFLM.GAFLIDEM /
 * GAFLRTRM) so @p extended and @p rtr are honoured, and CFDGAFLP1 routes
 * the match into RX FIFO ``rule.target_rx``.
 *
 * @param[in] channel CANFD channel index (0..1).
 * @param[in] rules   Array of @p count rules. Must not be NULL.
 * @param[in] count   Number of rules, 1..::k_ra8_canfd_afl_rule_capacity.
 *
 * @return ::ra8_err_t outcome.
 * @retval k_ra8_ok               All @p count rules programmed and re-locked.
 * @retval k_ra8_err_null_ptr     @p channel out of range or @p rules NULL.
 * @retval k_ra8_err_invalid_arg  @p count is zero or above capacity, a rule's
 *                               id/mask overflows the std/ext range, or a
 *                               rule's target_rx is out of range.
 *
 * @pre  @p rules points to at least @p count valid rule descriptors.
 * @pre  The global block is in GL_RESET: RNC0 and the CFDGAFL entries are
 *       RESET-only, and the AFL lookup engine only resamples the list on
 *       the next GL_RESET -> GL_OPERATION transition (HUM Ch 41.2.18
 *       p 2735, Ch 41.2.17 p 2734).
 * @post On success CFDGAFLCFG0.RNC0 == @p count and the first @p count
 *       CFDGAFL slots reflect the supplied rules.
 * @post CFDGAFLECTR.AFLDAE is cleared (window re-locked) on every return
 *       that reaches the write stage.
 *
 * @note Not thread-safe; single-threaded init / reconfigure context only.
 * @see  ra8_canfd_filter_set()  Single-rule variant that wraps the GL_RESET
 *       transition itself.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_canfd_set_accept_filter(uint8_t channel, const ra8_canfd_afl_rule_t* rules, uint8_t count);

/**
 * @brief Configure the CAN-FD bit-rate-switch (BRS) data-phase rate.
 *
 * @details
 * Re-runs the data-phase timing solver against PCLKA and updates
 * CFDC2[0].DCFG so that subsequent BRS frames sent on @p channel
 * use @p fast_bitrate during the payload phase.  See HUM Ch 41
 * "CFDCnDCFG" pp 2702-2867.
 *
 * @param[in] channel       Channel index (0..1).
 * @param[in] fast_bitrate  Data-phase bit rate in bits per second.
 *
 * @return ::ra8_err_t outcome.
 * @retval k_ra8_ok               DCFG updated.
 * @retval k_ra8_err_null_ptr     @p channel out of range.
 * @retval k_ra8_err_invalid_arg  @p fast_bitrate is zero or unsolvable.
 *
 * @pre  ::ra8_canfd_init succeeded for @p channel.
 * @pre  PCLKA frequency is reachable via ``ra8_cgc_get_clock_hz``.
 * @post CFDC2[0].DCFG reflects the new fast-phase timing triple.
 * @post Subsequent BRS frames switch to @p fast_bitrate after BRS bit.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_canfd_set_brs(uint8_t channel, uint32_t fast_bitrate);

/**
 * @struct ra8_canfd_tdc_cfg_t
 * @brief Transmitter Delay Compensation (TDC) configuration for one CANFD channel.
 *
 * @details
 * At CAN-FD data-phase bit rates above roughly 2 Mbps the transmitter
 * loop delay must be compensated or the secondary sample point drifts
 * outside the bit window. The RA8D2 RSCANFD block implements TDC via
 * three fields in CFDCnFDCFG (HUM Ch 41 "CFDCnFDCFG" p 2788):
 *
 *  - TDE (bit 16): global TDC enable gate.
 *  - TDCOC (bit 15): 0 = use the measured result from FDSTS.TDCR,
 *    1 = use the software-programmed TDCO value.
 *  - TDCO (bits [14:8]): a 7-bit offset applied to the secondary sample
 *    point (1 time-quanta resolution).
 *
 * Typical usage: set @p enable true and @p manual false to let the
 * hardware measure the loop delay automatically via FDSTS.TDCR.
 * Switch to @p manual true only when hardware measurement is unavailable
 * (test modes or very low data rates) and set @p offset to the known
 * loop-delay in time quanta.
 *
 * @invariant If @p enable is false, @p manual and @p offset are ignored.
 * @invariant @p offset must be in [0, ::k_ra8_canfd_tdc_offset_max] when
 *            @p enable is true.
 *
 * @code
 * ra8_canfd_tdc_cfg_t tdc = {
 *     .enable = true,
 *     .manual = false,
 *     .offset = 0U,
 * };
 * (void)ra8_canfd_set_tdc(0U, &tdc);
 * @endcode
 *
 * @see ra8_canfd_set_tdc()
 * @see ra8_fdcfg_mask_t
 */
typedef struct {
  bool    enable; /**< true = enable TDC (FDCFG.TDE = 1).                               */
  bool    manual; /**< true = use TDCO offset (FDCOC = 1), false = measured FDSTS.TDCR. */
  uint8_t offset; /**< TDCO offset in time quanta (0..::k_ra8_canfd_tdc_offset_max).    */
} ra8_canfd_tdc_cfg_t;

/**
 * @brief Configure Transmitter Delay Compensation on a CANFD channel.
 *
 * @details
 * Writes the TDE, TDCOC, and TDCO fields in CFDCnFDCFG to enable or
 * disable the transmitter delay compensation block, select the offset
 * source (hardware-measured vs. manual), and program the TDCO offset
 * value. All non-TDC bits (FDOE, REFE, CLOE, ESIC, EOCCFG) are
 * preserved by a read-modify-write.
 *
 * The write is bracketed by a CH_RESET / CH_OPERATION mode transition
 * (mirrors the ``ra8_canfd_set_bitrate`` contract: FDCFG is only
 * writable in CH_RESET or CH_HALT mode, per HUM Ch 41 "CFDCnFDCFG"
 * p 2788). On a transition timeout the function returns the timeout
 * error and the channel is left in the failing mode; the caller must
 * call ``ra8_canfd_deinit`` before attempting recovery.
 *
 * @param[in] channel  CANFD channel index (0..1).
 * @param[in] cfg      TDC configuration descriptor. Must not be NULL.
 *                     ``cfg->offset`` must be <= ::k_ra8_canfd_tdc_offset_max.
 *
 * @return ::ra8_err_t outcome.
 * @retval k_ra8_ok               CFDCnFDCFG updated; channel back in CH_OPERATION.
 * @retval k_ra8_err_null_ptr     @p channel is out of range or @p cfg is NULL.
 * @retval k_ra8_err_invalid_arg  ``cfg->offset`` exceeds the 7-bit TDCO cap (127).
 * @retval k_ra8_err_hw_timeout   Channel mode transition stalled.
 *
 * @pre  ::ra8_canfd_init returned ::k_ra8_ok for @p channel.
 * @pre  PCLKA / CANFDCLK is stable (block-clock guard in ::ra8_canfd_init).
 * @post On ::k_ra8_ok: FDCFG.TDE / TDCOC / TDCO reflect @p cfg.
 * @post On ::k_ra8_ok: channel is back in CH_OPERATION; TX/RX may resume.
 * @post Non-TDC FDCFG bits (FDOE, REFE, CLOE, ESIC, EOCCFG) are unchanged.
 * @post On error: the channel may be stuck in CH_RESET; caller must deinit.
 *
 * @note Not thread-safe; serialize with TX/RX operations.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_canfd_set_tdc(uint8_t channel, const ra8_canfd_tdc_cfg_t* cfg);

/**
 * @brief Select ISO 11898-1 vs non-ISO CAN-FD framing.
 *
 * @details
 * Writes CFDGFDCFG.NISO -- when the bit is clear the controller
 * uses the ISO 11898-1 stuff-count + CRC; when set it falls back
 * to the original Bosch non-ISO framing.  See HUM Ch 41 "CFDGFDCFG"
 * pp 2702-2867.
 *
 * @param[in] enable  true -> ISO mode, false -> non-ISO mode.
 *
 * @return ::ra8_err_t outcome.
 * @retval k_ra8_ok    Mode bit updated.
 *
 * @pre  At least one CANFD channel is initialized.
 * @pre  Controller is in global-reset before flipping the bit.
 * @post CFDGFDCFG.NISO reflects @p enable.
 * @post Subsequent CAN-FD frames use the selected framing.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_canfd_set_iso_mode(bool enable);

/**
 * @brief Enable a CFDC[0].CTR test mode (basic / listen-only / loopback).
 *
 * @details
 * Stamps CTME (bit 24) and CTMS[1:0] (bits [26:25]) in CFDC[0].CTR.
 * Per HUM Ch 41 "CFDCnCTR" p 2710 these bits are only writable while
 * the channel is in CH_HALT mode, so the helper:
 *   1. Drives the channel from CH_OPERATION (the post-init steady
 *      state) into CH_HALT via CHMDC = 10b and waits for CHLTSTS.
 *   2. Writes CTME=1 with the requested CTMS value.
 *   3. Returns the channel to CH_OPERATION via CHMDC = 00b.
 *
 * The internal-loopback mode (k_ra8_ctms_self_test_1, 11b) is the
 * documented way to validate the CANFD IP without a transceiver --
 * the controller routes every TX frame straight to its own RX
 * acceptance filter (HUM Ch 41 "Self-test mode 1 (Internal Loopback
 * mode)" p 2710).
 *
 * @param[in] channel  Channel index (0..1).
 * @param[in] mode     Desired CTMS selector (::ra8_ctms_mode_t).
 *
 * @return ::ra8_err_t outcome.
 * @retval k_ra8_ok               Test mode latched, channel back in operation.
 * @retval k_ra8_err_invalid_arg  @p channel or @p mode out of range.
 * @retval k_ra8_err_hw_timeout   Halt/operation status bit never asserted.
 *
 * @pre  ::ra8_canfd_init has returned k_ra8_ok for @p channel.
 * @pre  No outstanding TX/RX is in flight on @p channel.
 * @post CFDC[0].CTR.CTME=1 and CTMS=@p mode.
 * @post Channel is back in CH_OPERATION ready to TX/RX.
 *
 * @note Not thread-safe; caller must serialize with TX/RX.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_canfd_set_test_mode(uint8_t channel, ra8_ctms_mode_t mode);

/**
 * @brief Put the CANFD channel into MSTP-gated stop.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_canfd_enter_stop(uint8_t channel);

/**
 * @brief Exit MSTP-gated stop.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_canfd_exit_stop(uint8_t channel);

#ifdef __cplusplus
}
#endif
