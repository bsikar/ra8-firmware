/**
 * @file ra_canfd.h
 * @brief CANFD Lite driver (bit-timing, TX, RX, error state)
 *
 * @details
 * Minimal driver surface over the RA8D2 CANFD Lite controller --
 * enough to bring a channel up at a chosen nominal bit rate, queue a
 * CAN 2.0B / CAN-FD frame into the TX message buffer, poll the RX
 * FIFO for inbound frames, and read the TX/RX error counters.
 *
 * DMA hooks, interrupt delivery, and the full acceptance-filter bank
 * are deferred until there is a real consumer that needs them.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8d2_canfd_regs.h"
#include "ra_err.h"

/**
 * @struct ra_canfd_frame_t
 * @brief A single CAN / CAN-FD frame as seen by the driver public API.
 *
 * @details
 * `id` holds an 11-bit standard or 29-bit extended identifier (masked
 * to `k_ra_canfd_id_std_mask` / `k_ra_canfd_id_ext_mask` on send).
 * `dlc` is the raw DLC nibble (0..15) -- callers passing a length
 * longer than 8 bytes must also set `is_fd`.
 *
 * cppcheck cannot see tests/ so it flags every field as unused;
 * each member is read in ``ra_canfd_transmit`` and
 * ``ra_canfd_receive`` in ``libs/ra_hal/src/ra_canfd.c``.
 *
 * @invariant `dlc <= 15`
 * @invariant If `is_extended == 0`, `id` fits in 11 bits.
 * @invariant If `is_brs`, the channel must have been initialized with
 *            a data bit rate > nominal bit rate.
 *
 * @note The `data` array is sized for the worst case (64-byte CAN-FD
 *       payload). Classic CAN frames only touch the first 8 bytes.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint32_t id;                              /**< Arbitration-ID field.                   */
  uint8_t  dlc;                             /**< DLC code (0..15). See CAN-FD DLC table. */
  uint8_t  is_extended;                     /**< 0 = 11-bit ID, 1 = 29-bit ID.           */
  uint8_t  is_fd;                           /**< 0 = classic CAN, 1 = CAN-FD.            */
  uint8_t  is_brs;                          /**< 1 = bit-rate switch in data phase.      */
  uint8_t  data[k_ra_canfd_data_bytes_max]; /**< Payload bytes.                          */
} ra_canfd_frame_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @brief Take a CANFD channel out of reset into operation mode.
 *
 * @param[in] channel Channel index (0..1).
 * @return `k_ra_ok` on success, `k_ra_err_null_ptr` if `channel` out of range.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_canfd_init(uint8_t channel);

/**
 * @brief Put the CANFD channel back into reset.
 *
 * @param[in] channel Channel index (0..1).
 * @return `k_ra_ok` on success, `k_ra_err_null_ptr` if `channel` out of range.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_canfd_deinit(uint8_t channel);

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
 * @return `k_ra_err_null_ptr` if `channel` out of range.
 * @return `k_ra_err_invalid_arg` if either bit rate is zero or does
 *         not resolve to a valid prescaler / timing triple.
 * @return `k_ra_ok` on success.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_canfd_set_bitrate(uint8_t channel, uint32_t bitrate_bps, uint32_t data_bitrate_bps);

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
 * @return `k_ra_err_null_ptr`   if `channel` out of range or `frame == NULL`.
 * @return `k_ra_err_invalid_arg` on bad DLC, oversized ID, or inconsistent flags.
 * @return `k_ra_ok` on success.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_canfd_transmit(uint8_t channel, const ra_canfd_frame_t* frame);

/**
 * @brief Poll the RX FIFO for the next available frame.
 *
 * @details
 * Non-blocking: on an empty FIFO the call returns `k_ra_err_no_data`
 * immediately. On success the frame is popped (by writing the pointer
 * control register) before returning.
 *
 * @param[in]  channel   Channel index (0..1).
 * @param[out] out_frame Destination frame. Must not be NULL.
 *
 * @return `k_ra_err_null_ptr`  if `channel` out of range or `out_frame == NULL`.
 * @return `k_ra_err_no_data`   if the RX FIFO is empty.
 * @return `k_ra_ok`            on success.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_canfd_receive(uint8_t channel, ra_canfd_frame_t* out_frame);

/**
 * @brief Read the TX and RX error counters.
 *
 * @param[in]  channel Channel index (0..1).
 * @param[out] tx_err  TEC counter value. Must not be NULL.
 * @param[out] rx_err  REC counter value. Must not be NULL.
 *
 * @return `k_ra_err_null_ptr` if any pointer is NULL or channel out of range.
 * @return `k_ra_ok`           on success.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_canfd_get_error_state(uint8_t channel, uint8_t* tx_err, uint8_t* rx_err);

/**
 * @typedef ra_canfd_event_fn_t
 * @brief CANFD event callback.
 */
typedef void (*ra_canfd_event_fn_t)(void* ctx, uint8_t channel, uint32_t status_mask);

/**
 * @brief Read the channel status register (CFDCnSTS).
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_canfd_get_status(uint8_t channel, uint32_t* out_mask);

/**
 * @brief Clear error flags in CFDCnERFL.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_canfd_clear_status(uint8_t channel, uint32_t mask);

/**
 * @brief Attach a callback for CANFD events (shared across channels).
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_canfd_attach_handler(ra_canfd_event_fn_t fn, void* ctx);

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
void ra_canfd_dispatch(uint8_t channel);

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
 * @param[in] filter_id  Filter index (0..k_ra_canfd_afl_total - 1).
 * @param[in] accept_id  Raw arbitration ID to accept (11- or 29-bit).
 * @param[in] mask       Bit mask -- bits clear are "don't care".
 * @param[in] dlc        DLC code packed into CFDGAFL.P1 (0..15).
 *
 * @return ::ra_err_t outcome.
 * @retval k_ra_ok               Filter slot programmed.
 * @retval k_ra_err_invalid_arg  @p filter_id, @p accept_id, or @p dlc out
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
[[nodiscard]] ra_err_t
ra_canfd_filter_set(uint16_t filter_id, uint32_t accept_id, uint32_t mask, uint8_t dlc);

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
 * @return ::ra_err_t outcome.
 * @retval k_ra_ok               DCFG updated.
 * @retval k_ra_err_null_ptr     @p channel out of range.
 * @retval k_ra_err_invalid_arg  @p fast_bitrate is zero or unsolvable.
 *
 * @pre  ::ra_canfd_init succeeded for @p channel.
 * @pre  PCLKA frequency is reachable via ``ra_cgc_get_clock_hz``.
 * @post CFDC2[0].DCFG reflects the new fast-phase timing triple.
 * @post Subsequent BRS frames switch to @p fast_bitrate after BRS bit.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_canfd_set_brs(uint8_t channel, uint32_t fast_bitrate);

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
 * @return ::ra_err_t outcome.
 * @retval k_ra_ok    Mode bit updated.
 *
 * @pre  At least one CANFD channel is initialized.
 * @pre  Controller is in global-reset before flipping the bit.
 * @post CFDGFDCFG.NISO reflects @p enable.
 * @post Subsequent CAN-FD frames use the selected framing.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_canfd_set_iso_mode(bool enable);

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
 * The internal-loopback mode (k_ra_ctms_self_test_1, 11b) is the
 * documented way to validate the CANFD IP without a transceiver --
 * the controller routes every TX frame straight to its own RX
 * acceptance filter (HUM Ch 41 "Self-test mode 1 (Internal Loopback
 * mode)" p 2710).
 *
 * @param[in] channel  Channel index (0..1).
 * @param[in] mode     Desired CTMS selector (::ra_ctms_mode_t).
 *
 * @return ::ra_err_t outcome.
 * @retval k_ra_ok               Test mode latched, channel back in operation.
 * @retval k_ra_err_invalid_arg  @p channel or @p mode out of range.
 * @retval k_ra_err_hw_timeout   Halt/operation status bit never asserted.
 *
 * @pre  ::ra_canfd_init has returned k_ra_ok for @p channel.
 * @pre  No outstanding TX/RX is in flight on @p channel.
 * @post CFDC[0].CTR.CTME=1 and CTMS=@p mode.
 * @post Channel is back in CH_OPERATION ready to TX/RX.
 *
 * @note Not thread-safe; caller must serialize with TX/RX.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_canfd_set_test_mode(uint8_t channel, ra_ctms_mode_t mode);

/**
 * @brief Put the CANFD channel into MSTP-gated stop.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_canfd_enter_stop(uint8_t channel);

/**
 * @brief Exit MSTP-gated stop.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_canfd_exit_stop(uint8_t channel);

#ifdef RA_SIMULATOR_MODE
/**
 * @brief Inject a raw frame into the simulator's RX FIFO for fuzzing.
 *
 * @details
 * Test-only veneer compiled when ``RA_SIMULATOR_MODE`` is defined.
 * Stamps the supplied bytes into ``CFDRF[0].ID`` / ``PTR`` /
 * ``FDSTS`` / ``DF[]`` and clears ``CFDRFSTS[0]`` so that a follow-up
 * call to ::ra_canfd_receive consumes the frame. Used by the
 * libFuzzer harness in ``tests/fuzz/fuzz_ra_canfd.c``.
 *
 * @param[in] channel    Channel index.
 * @param[in] id_word    Raw value to drop into ``CFDRF[0].ID``.
 * @param[in] ptr_word   Raw value to drop into ``CFDRF[0].PTR``.
 * @param[in] fdsts_word Raw value to drop into ``CFDRF[0].FDSTS``.
 * @param[in] data       Source bytes for ``CFDRF[0].DF[]``; may be NULL.
 * @param[in] data_len   Number of bytes from @p data to copy (clamped
 *                       to the 64-byte CAN-FD payload cap).
 *
 * @retval k_ra_ok            Frame staged.
 * @retval k_ra_err_null_ptr  Channel out of range.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_canfd_test_inject_frame(uint8_t        channel,
                                                  uint32_t       id_word,
                                                  uint32_t       ptr_word,
                                                  uint32_t       fdsts_word,
                                                  const uint8_t* data,
                                                  uint32_t       data_len);
#endif /* RA_SIMULATOR_MODE */

#ifdef __cplusplus
}
#endif
