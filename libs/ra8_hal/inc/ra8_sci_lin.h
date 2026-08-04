/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_sci_lin.h
 * @brief LIN (Local Interconnect Network) commander + responder driver on SCI_B
 * @ingroup grp_hal_comms
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * LIN is a single-wire, single-commander serial protocol layered on top
 * of a standard UART. The commander node drives every frame header; it
 * emits a BREAK field (at least 13 dominant bit-times), a SYNC byte
 * (0x55), and a protected identifier (PID = a 6-bit frame id plus two
 * parity bits). The header is then followed by a response phase -- data
 * bytes and a checksum -- published by whichever node the frame id
 * assigns as the transmitter (commander or responder).
 *
 * This driver implements **both** LIN roles on the SCI_B Simple-LIN
 * sub-mode:
 *
 * - **Commander** (``k_ra8_sci_lin_role_commander``): hardware GENERATES the
 *   break field via the break-field timer (XCR0.TCSS clock, XCR0.BFE,
 *   XCR2.BFLW length, XCR1.TCST trigger); SYNC and PID are clocked out as
 *   ordinary UART frames.
 * - **Responder** (``k_ra8_sci_lin_role_responder``): hardware DETECTS the
 *   break field via Start-Frame detection (XCR1.SDST) and reports it
 *   through XSR0.BFDF; the sync field is measured by the bit-rate
 *   measurement counter (XCR1.BMEN -> XSR1.TCNT).
 *
 * @par Inclusive-terminology note
 * The LIN specification's commander node is the bus controller and its
 * subordinate nodes are termed "responders" here. The legacy LIN node
 * words are avoided in favour of Commander / Responder, per the project
 * terminology standard.
 *
 * This driver targets the **SCI_B** Simple-LIN sub-mode (HUM Ch 38.11
 * "Simple LIN Mode", p 2338; register-level break generation via
 * XCR0/XCR1/XCR2 and detection via XSR0/XSR1/XFCLR). It builds on the
 * async-UART bring-up in ``ra8_sci.h``: ``ra8_sci_lin_init`` configures the
 * baud / framing through the base UART path and then switches CCR3.MOD to
 * Simple LIN and programs the role-specific break-field timer / detector.
 *
 * This header includes ``ra8_sci.h`` one-directionally for the base
 * configuration type (``ra8_sci_cfg_t``). ``ra8_sci.h`` does NOT include this
 * header back; consumers that need the LIN API include ``ra8_sci_lin.h``
 * directly.
 *
 * ## API surface
 *
 * Lifecycle:
 * - ``ra8_sci_lin_init``          -- configure SCI_B for a LIN role
 *
 * Commander frame generation:
 * - ``ra8_sci_lin_send_break``    -- emit the break field
 * - ``ra8_sci_lin_send_header``   -- break + SYNC (0x55) + PID
 *
 * Responder frame detection:
 * - ``ra8_sci_lin_break_detected``-- poll XSR0.BFDF once (non-blocking)
 * - ``ra8_sci_lin_wait_break``    -- block until a break field is detected
 * - ``ra8_sci_lin_clear_status``  -- clear the XSR0 LIN latches via XFCLR
 * - ``ra8_sci_lin_check_header``  -- pure: validate a received SYNC + PID
 *
 * Response (data) phase, either role:
 * - ``ra8_sci_lin_send_response`` -- publish data bytes + checksum
 * - ``ra8_sci_lin_read_response`` -- subscribe: read data bytes + checksum
 * - ``ra8_sci_lin_check_response``-- pure: verify a received response checksum
 *
 * Pure helpers:
 * - ``ra8_sci_lin_pid``           -- pure: 6-bit id -> protected identifier
 * - ``ra8_sci_lin_checksum``      -- pure: classic / enhanced LIN checksum
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_sci.h"

/* =============================================================================
 * Types
 * =============================================================================
 */

/**
 * @enum ra8_sci_lin_limits_t
 * @brief Public range limits for the LIN frame identifier.
 *
 * @details The LIN protected identifier carries a 6-bit frame id, so the
 * largest legal value passed to ``ra8_sci_lin_send_header`` is 0x3F (63).
 */
typedef enum : uint8_t {
  k_ra8_sci_lin_id_max = 0x3FU, /**< Highest legal 6-bit LIN frame id. */
} ra8_sci_lin_limits_t;

/**
 * @enum ra8_sci_lin_timer_clk_t
 * @brief Break-field timer clock divider (maps to SCI_B XCR0.TCSS[1:0]).
 *
 * @details Selects the divider applied to the Simple-LIN module timer
 * clock (TCLK) that times the break field. The enumerator values equal
 * the on-chip XCR0.TCSS encodings (HUM Ch 38.2.14, p 2221), so no
 * translation table is needed.
 */
typedef enum : uint8_t {
  k_ra8_sci_lin_clk_div4  = 1U, /**< Break timer clock = TCLK / 4.  */
  k_ra8_sci_lin_clk_div16 = 2U, /**< Break timer clock = TCLK / 16. */
  k_ra8_sci_lin_clk_div64 = 3U, /**< Break timer clock = TCLK / 64. */
} ra8_sci_lin_timer_clk_t;

/**
 * @enum ra8_sci_lin_role_t
 * @brief Selects whether the channel drives (commander) or detects
 *        (responder) the LIN frame header.
 *
 * @details A LIN bus has exactly one commander that generates every frame
 * header (break + sync + PID) and any number of responders that detect it.
 * The two roles program different Simple-LIN control bits at init: the
 * commander arms the break-field output timer (XCR1.TCST is pulsed per
 * frame), while the responder arms Start-Frame detection (XCR1.SDST) plus
 * bit-rate measurement (XCR1.BMEN) so the hardware flags an inbound break
 * in XSR0.BFDF and measures the sync field into XSR1.TCNT.
 */
typedef enum : uint8_t {
  k_ra8_sci_lin_role_commander = 0U, /**< Generate the header (bus controller). */
  k_ra8_sci_lin_role_responder = 1U, /**< Detect the header (subordinate node). */
} ra8_sci_lin_role_t;

/**
 * @enum ra8_sci_lin_checksum_mode_t
 * @brief Selector for the classic vs. enhanced LIN checksum.
 *
 * @details Classic (LIN 1.x) sums only the data bytes; enhanced (LIN 2.x)
 * folds the protected identifier into the sum as well. Both then take the
 * inverted modulo-255 sum.
 */
typedef enum : uint8_t {
  k_ra8_sci_lin_checksum_classic  = 0U, /**< LIN 1.x: sum of data bytes only.  */
  k_ra8_sci_lin_checksum_enhanced = 1U, /**< LIN 2.x: sum of PID + data bytes. */
} ra8_sci_lin_checksum_mode_t;

/**
 * @struct ra8_sci_lin_cfg_t
 * @brief Configuration descriptor for ``ra8_sci_lin_init``.
 *
 * @details
 * Wraps the base async-UART configuration (baud / framing) used to bring
 * the channel up before the mode switch, plus the Simple-LIN-specific
 * parameters: the node role, the break-field timer clock divider, and the
 * break-field length register value. ``break_field_len`` sizes the break
 * generated by a commander and the minimum break a responder's detector
 * accepts; both use the same TCSS timer clock.
 *
 * cppcheck cannot see tests/ so it flags every field as unused; each
 * member is read in ``ra8_sci_lin_init`` in
 * ``libs/ra8_hal/src/ra8_sci_lin.c``.
 *
 * @invariant ``break_field_len`` <= ``k_ra8_sci_xcr2_bflw_max`` (0xFFFE).
 */
typedef struct {
  ra8_sci_cfg_t           uart;            /**< Base async-UART baud + framing. */
  ra8_sci_lin_role_t      role;            /**< Commander (generate) or responder
                                               (detect) the frame header. */
  ra8_sci_lin_timer_clk_t timer_clk;       /**< Break-field timer clock (TCSS). */
  uint16_t                break_field_len; /**< XCR2.BFLW: dominant time =
                                               (break_field_len + 1) x timer
                                               clock. >= 13 bit-times per the
                                               LIN standard; 0xFFFF prohibited. */
} ra8_sci_lin_cfg_t;

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Configure an SCI_B channel for a LIN role (commander or responder).
 *
 * @details
 * First brings the channel up as an async UART (``ra8_sci_init`` with
 * ``cfg->uart``: MSTP gate, baud, framing, TE/RE). It then drops CCR0,
 * switches CCR3.MOD to Simple LIN (110b) while preserving the framing bits
 * the base init programmed, programs the break-field timer clock
 * (XCR0.TCSS) and break-field enable (XCR0.BFE), sets the break-field
 * length (XCR2.BFLW), and re-enables CCR0.TE + CCR0.RE. The XCR1 program
 * depends on ``cfg->role``: a commander leaves XCR1 idle (TCST is pulsed
 * per frame by ``ra8_sci_lin_send_break``); a responder sets XCR1.SDST
 * (Start-Frame detection) and XCR1.BMEN (bit-rate measurement) so the
 * hardware flags an inbound break in XSR0.BFDF (HUM Ch 38.11.2 p 2341).
 *
 * @param[in] channel SCI channel number (0..9).
 * @param[in] cfg     Non-NULL LIN configuration descriptor.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               Channel configured for the requested role.
 * @retval k_ra8_err_null_ptr     ``cfg`` was NULL or ``channel`` out of range.
 * @retval k_ra8_err_invalid_arg  ``role`` undefined, ``timer_clk`` not a defined
 *                               divider, or ``break_field_len`` exceeds 0xFFFE.
 * @retval k_ra8_err_hw_init_failed The underlying ``ra8_sci_init`` failed.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre ``ra8_mstp_init`` has been called.
 * @post On success, CCR3.MOD == Simple LIN and the break-field timer is
 *       programmed from ``cfg``.
 * @post On success, CCR0.TE and CCR0.RE are set, and XCR1 reflects the role.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_sci_lin_send_header
 * @see ra8_sci_lin_wait_break
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sci_lin_init(uint8_t channel, const ra8_sci_lin_cfg_t* cfg);

/* =============================================================================
 * Frame transmission
 * =============================================================================
 */

/**
 * @brief Emit a LIN break field on the channel's TXD line.
 *
 * @details
 * Writes 1 to XCR1.TCST, which starts the hardware break-field timer; the
 * SCI_B holds TXD dominant for the ``(BFLW + 1) x`` timer-clock period set
 * at init and self-clears TCST when the field completes. The call then
 * blocks (bounded spin) until TCST reads back clear. On the host
 * (``RA8_OFF_TARGET``) the timer drain is not modelled, so the wait
 * returns success immediately.
 *
 * @param[in] channel SCI channel number (0..9).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok            Break field emitted (or fake stub).
 * @retval k_ra8_err_null_ptr  ``channel`` out of range.
 * @retval k_ra8_err_hw_timeout TCST did not self-clear within the budget.
 *
 * @pre Channel previously configured via ``ra8_sci_lin_init``.
 * @pre IRQs masked or single-threaded transmit context.
 * @post On success, the break-field timer is idle (XCR1.TCST clear).
 * @post No data byte has been pushed to TDR by this call.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_sci_lin_send_header
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sci_lin_send_break(uint8_t channel);

/**
 * @brief Transmit a full LIN header: break + SYNC (0x55) + PID.
 *
 * @details
 * Emits the break field (``ra8_sci_lin_send_break``), then sends the SYNC
 * byte 0x55 and the protected identifier as two ordinary UART frames via
 * ``ra8_sci_putc_polling``. The PID is computed from ``id`` with
 * ``ra8_sci_lin_pid`` (6-bit id plus the two LIN parity bits).
 *
 * @param[in] channel SCI channel number (0..9).
 * @param[in] id      6-bit LIN frame identifier (0..63).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Header transmitted.
 * @retval k_ra8_err_null_ptr    ``channel`` out of range.
 * @retval k_ra8_err_invalid_arg ``id`` exceeds 63.
 * @retval k_ra8_err_hw_timeout  A TDRE poll or the break wait timed out.
 *
 * @pre Channel previously configured via ``ra8_sci_lin_init``.
 * @pre ``id`` <= ``k_ra8_sci_lin_id_max``.
 * @post On success, the break field, SYNC, and PID have been clocked out.
 * @post TDR holds the protected identifier (the final byte written).
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_sci_lin_pid
 * @see ra8_sci_lin_send_break
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sci_lin_send_header(uint8_t channel, uint8_t id);

/* =============================================================================
 * Responder frame detection
 * =============================================================================
 */

/**
 * @brief Report whether the responder's break-field detector has fired.
 *
 * @details
 * Reads XSR0.BFDF once (non-blocking) and writes its state to
 * ``*out_detected``. On a responder channel the Simple-LIN Start-Frame
 * detector sets BFDF when it sees a dominant span at least as long as the
 * configured break-field length; the flag stays set until cleared through
 * ``ra8_sci_lin_clear_status``. Reads no data and modifies no hardware.
 *
 * @param[in]  channel      SCI channel number (0..9).
 * @param[out] out_detected Receives true if XSR0.BFDF is set, else false.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Flag state written to ``*out_detected``.
 * @retval k_ra8_err_null_ptr    ``out_detected`` is NULL or ``channel`` out of
 *                              range.
 *
 * @pre ``out_detected`` is non-NULL.
 * @pre Channel previously configured as a responder via ``ra8_sci_lin_init``.
 * @post ``*out_detected`` reflects the current XSR0.BFDF bit.
 * @post No hardware register is modified.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_sci_lin_wait_break
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sci_lin_break_detected(uint8_t channel, bool* out_detected);

/**
 * @brief Block until the responder detects a break field (XSR0.BFDF set).
 *
 * @details
 * Spins on XSR0.BFDF with a bounded budget; returns as soon as the break
 * field is detected. On the host (``RA8_OFF_TARGET``) the poll runs
 * against the programmable MMIO seam, so a test pre-stages BFDF or arms a
 * timeout. Does not clear the flag -- call ``ra8_sci_lin_clear_status``
 * before waiting for the next frame.
 *
 * @param[in] channel SCI channel number (0..9).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok             A break field was detected within the budget.
 * @retval k_ra8_err_null_ptr   ``channel`` out of range.
 * @retval k_ra8_err_hw_timeout No break field detected within the budget.
 *
 * @pre Channel previously configured as a responder via ``ra8_sci_lin_init``.
 * @pre IRQs masked or single-threaded receive context.
 * @post On success, XSR0.BFDF was observed set (still latched).
 * @post No data register is touched.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_sci_lin_break_detected
 * @see ra8_sci_lin_clear_status
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sci_lin_wait_break(uint8_t channel);

/**
 * @brief Clear the Simple-LIN status latches (XSR0) via XFCLR.
 *
 * @details
 * Writes the clear-all-LIN mask to XFCLR, which clears every W1C XSR0
 * latch (BFDF, AEDF, COF, bus-conflict, and the control-field match
 * flags). A responder calls this after consuming one frame's break /
 * sync detection so the detector is armed clean for the next header.
 *
 * @param[in] channel SCI channel number (0..9).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok            LIN status latches cleared.
 * @retval k_ra8_err_null_ptr  ``channel`` out of range.
 *
 * @pre Channel previously configured via ``ra8_sci_lin_init``.
 * @pre IRQs masked or single-threaded context.
 * @post XSR0.BFDF and the other LIN latches read back clear.
 * @post No data register is touched.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_sci_lin_wait_break
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sci_lin_clear_status(uint8_t channel);

/* =============================================================================
 * Response (data) phase -- either role
 * =============================================================================
 */

/**
 * @brief Publish a LIN response: data bytes followed by the checksum.
 *
 * @details
 * Computes the classic or enhanced checksum over ``data`` (folding ``pid``
 * in for the enhanced mode) via ``ra8_sci_lin_checksum``, then clocks the
 * ``len`` data bytes and the checksum byte out as ordinary UART frames
 * with ``ra8_sci_putc_polling``. Used by the transmitting node of a frame:
 * the commander for a commander-to-responder frame, or a responder for a
 * responder-to-commander frame. The caller must already have sent (as
 * commander) or detected (as responder) the header.
 *
 * @param[in] channel SCI channel number (0..9).
 * @param[in] mode    Classic (data only) or enhanced (PID + data) checksum.
 * @param[in] pid     Protected identifier, folded into the enhanced sum.
 * @param[in] data    Data-field byte buffer; non-NULL when ``len`` > 0.
 * @param[in] len     Number of data bytes (1..8 for standard LIN).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Data + checksum clocked out.
 * @retval k_ra8_err_null_ptr    ``channel`` out of range, or ``data`` NULL with
 *                              ``len`` > 0.
 * @retval k_ra8_err_invalid_arg ``mode`` undefined or ``len`` is 0 or > 8.
 * @retval k_ra8_err_hw_timeout  A TDRE poll timed out mid-frame.
 *
 * @pre Channel previously configured via ``ra8_sci_lin_init``.
 * @pre ``data`` is non-NULL and ``len`` is in 1..8.
 * @post On success, the data bytes and the checksum byte have been sent.
 * @post TDR holds the checksum (the final byte written).
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_sci_lin_read_response
 * @see ra8_sci_lin_checksum
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sci_lin_send_response(uint8_t                     channel,
                                                  ra8_sci_lin_checksum_mode_t mode,
                                                  uint8_t                     pid,
                                                  const uint8_t*              data,
                                                  uint8_t                     len);

/**
 * @brief Read a LIN response: ``len`` data bytes plus one checksum byte.
 *
 * @details
 * Drains ``len`` data bytes into ``out_data`` and the trailing checksum
 * byte into ``*out_checksum``, each via ``ra8_sci_putc_polling``'s receive
 * peer ``ra8_sci_getc_polling``. This is a thin I/O primitive: it does NOT
 * validate the checksum -- the caller passes ``out_data`` / ``*out_checksum``
 * to ``ra8_sci_lin_check_response`` to verify. Used by the receiving node
 * of a frame (the subscriber).
 *
 * @param[in]  channel      SCI channel number (0..9).
 * @param[out] out_data     Buffer receiving ``len`` data bytes.
 * @param[in]  len          Number of data bytes to read (1..8).
 * @param[out] out_checksum Receives the trailing checksum byte.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              ``len`` data bytes + checksum read.
 * @retval k_ra8_err_null_ptr    ``channel`` out of range, or ``out_data`` /
 *                              ``out_checksum`` is NULL.
 * @retval k_ra8_err_invalid_arg ``len`` is 0 or > 8.
 * @retval k_ra8_err_hw_timeout  An RDRF poll timed out mid-frame.
 *
 * @pre Channel previously configured via ``ra8_sci_lin_init``.
 * @pre ``out_data`` holds at least ``len`` bytes; ``out_checksum`` non-NULL.
 * @post On success, ``out_data[0..len-1]`` and ``*out_checksum`` are filled.
 * @post No hardware control register is modified.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_sci_lin_check_response
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_sci_lin_read_response(uint8_t channel, uint8_t* out_data, uint8_t len, uint8_t* out_checksum);

/* =============================================================================
 * Pure helpers
 * =============================================================================
 */

/**
 * @brief Validate a received LIN header: SYNC byte + protected identifier.
 *
 * @details
 * A responder accepts a header only when BOTH the SYNC field equals 0x55
 * AND the received PID's parity bits match the parity recomputed from its
 * low 6 bits. This pure predicate performs exactly that compound check,
 * writing the extracted 6-bit frame id to ``*out_id`` and the accept
 * decision to ``*out_valid``. Pure: touches no hardware register.
 *
 * @param[in]  sync      The received SYNC field byte (must be 0x55).
 * @param[in]  pid       The received protected-identifier byte.
 * @param[out] out_id    Receives the low 6 bits of ``pid`` (the frame id).
 * @param[out] out_valid Receives true iff SYNC == 0x55 and PID parity is OK.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok           Decision written to ``*out_valid`` / ``*out_id``.
 * @retval k_ra8_err_null_ptr ``out_id`` or ``out_valid`` is NULL.
 *
 * @pre ``out_id`` is non-NULL.
 * @pre ``out_valid`` is non-NULL.
 * @post ``*out_id`` == ``pid & 0x3F``.
 * @post ``*out_valid`` is true only for a well-formed SYNC + PID pair.
 *
 * @note Thread safety: pure function; safe to call concurrently.
 * @see ra8_sci_lin_pid
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_sci_lin_check_header(uint8_t sync, uint8_t pid, uint8_t* out_id, bool* out_valid);

/**
 * @brief Verify a received LIN response checksum against the data field.
 *
 * @details
 * Recomputes the classic or enhanced checksum over ``data`` (and ``pid``
 * for the enhanced mode) with ``ra8_sci_lin_checksum`` and compares it to
 * the ``received`` checksum byte, writing the match result to
 * ``*out_valid``. Pure: touches no hardware register. Pair with
 * ``ra8_sci_lin_read_response`` to validate an inbound response.
 *
 * @param[in]  mode      Classic (data only) or enhanced (PID + data).
 * @param[in]  pid       Protected identifier, folded in for enhanced mode.
 * @param[in]  data      Data-field byte buffer; non-NULL when ``len`` > 0.
 * @param[in]  len       Number of data bytes (0..8).
 * @param[in]  received  The checksum byte received on the wire.
 * @param[out] out_valid Receives true iff the recomputed checksum matches.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Decision written to ``*out_valid``.
 * @retval k_ra8_err_null_ptr    ``out_valid`` is NULL, or ``data`` NULL with
 *                              ``len`` > 0.
 * @retval k_ra8_err_invalid_arg ``mode`` is not a defined checksum mode.
 *
 * @pre ``out_valid`` is non-NULL.
 * @pre ``data`` is non-NULL whenever ``len`` > 0.
 * @post ``*out_valid`` is true only when the checksum recomputes exactly.
 * @post No input buffer is modified.
 *
 * @note Thread safety: pure function; safe to call concurrently.
 * @see ra8_sci_lin_read_response
 * @see ra8_sci_lin_checksum
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sci_lin_check_response(ra8_sci_lin_checksum_mode_t mode,
                                                   uint8_t                     pid,
                                                   const uint8_t*              data,
                                                   uint8_t                     len,
                                                   uint8_t                     received,
                                                   bool*                       out_valid);

/**
 * @brief Compute the LIN protected identifier (PID) from a frame id.
 *
 * @details
 * The PID packs the 6-bit frame id in bits 0..5 plus two parity bits:
 * ``P0 = ID0 ^ ID1 ^ ID2 ^ ID4`` in bit 6, and
 * ``P1 = NOT(ID1 ^ ID3 ^ ID4 ^ ID5)`` in bit 7 (LIN 2.x parity). Any caller
 * bits above bit 5 are masked off before the parity is computed, so the
 * function is total. Pure: touches no hardware register.
 *
 * @param[in] id LIN frame identifier; only the low 6 bits are significant.
 *
 * @return The 8-bit protected identifier.
 *
 * @pre ``id`` is a LIN frame identifier (low 6 bits significant).
 * @pre No hardware preconditions (pure function).
 * @post The low 6 bits of the result equal ``id & 0x3F``.
 * @post Bits 6 and 7 hold the computed parity bits P0 and P1.
 *
 * @note Thread safety: pure function; safe to call concurrently.
 * @see ra8_sci_lin_send_header
 * @since 0.1.0
 */
[[nodiscard]] uint8_t ra8_sci_lin_pid(uint8_t id);

/**
 * @brief Compute a classic or enhanced LIN checksum over a data field.
 *
 * @details
 * Sums the data bytes (and, for ``k_ra8_sci_lin_checksum_enhanced``, the
 * protected identifier ``pid``) into an accumulator, folds the carry bits
 * back into the low byte to form the modulo-255 sum, and returns the
 * one's complement of that sum. Pure: touches no hardware register.
 *
 * @param[in]  mode         Classic (data only) or enhanced (PID + data).
 * @param[in]  pid          Protected identifier, folded in only when
 *                          ``mode == k_ra8_sci_lin_checksum_enhanced``.
 * @param[in]  data         Data-field byte buffer; may be NULL only when
 *                          ``len == 0``.
 * @param[in]  len          Number of data bytes (0..8 for standard LIN).
 * @param[out] out_checksum Receives the 8-bit checksum on success.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Checksum computed.
 * @retval k_ra8_err_null_ptr    ``out_checksum`` is NULL, or ``data`` is NULL
 *                              while ``len`` > 0.
 * @retval k_ra8_err_invalid_arg ``mode`` is not a defined checksum mode.
 *
 * @pre ``out_checksum`` is non-NULL.
 * @pre ``data`` is non-NULL whenever ``len`` > 0.
 * @post On success, ``*out_checksum`` holds the inverted modulo-255 sum.
 * @post No input buffer is modified.
 *
 * @note Thread safety: pure function; safe to call concurrently.
 * @see ra8_sci_lin_pid
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sci_lin_checksum(ra8_sci_lin_checksum_mode_t mode,
                                             uint8_t                     pid,
                                             const uint8_t*              data,
                                             uint8_t                     len,
                                             uint8_t*                    out_checksum);

#ifdef __cplusplus
}
#endif
