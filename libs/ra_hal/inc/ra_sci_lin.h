/**
 * @file ra_sci_lin.h
 * @brief LIN (Local Interconnect Network) commander-mode driver on SCI_B
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * LIN is a single-wire, single-commander serial protocol layered on top
 * of a standard UART. The commander node drives every frame; it emits a
 * BREAK field (at least 13 dominant bit-times), a SYNC byte (0x55), and a
 * protected identifier (PID = a 6-bit frame id plus two parity bits),
 * followed by the responder's data bytes and a checksum.
 *
 * @par Inclusive-terminology note
 * The LIN specification's commander node is the bus controller; this
 * driver implements only that role. Subordinate nodes are termed
 * "responders" here. The legacy LIN node words are avoided in favour of
 * Commander / Responder, per the project terminology standard.
 *
 * This driver targets the **SCI_B** Simple-LIN sub-mode (HUM Ch 38,
 * register-level break-field generation via XCR0/XCR1/XCR2). It builds on
 * the async-UART bring-up in ``ra_sci.h``: ``ra_sci_lin_init`` configures
 * the baud / framing through the base UART path and then switches CCR3.MOD
 * to Simple LIN and programs the break-field timer.
 *
 * This header includes ``ra_sci.h`` one-directionally for the base
 * configuration type (``ra_sci_cfg_t``). ``ra_sci.h`` does NOT include this
 * header back; consumers that need the LIN API include ``ra_sci_lin.h``
 * directly.
 *
 * ## API surface
 *
 * - ``ra_sci_lin_init``        -- configure SCI_B for LIN commander mode
 * - ``ra_sci_lin_send_break``  -- emit the break field
 * - ``ra_sci_lin_send_header`` -- break + SYNC (0x55) + PID
 * - ``ra_sci_lin_pid``         -- pure: 6-bit id -> protected identifier
 * - ``ra_sci_lin_checksum``    -- pure: classic / enhanced LIN checksum
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra_err.h"
#include "ra_sci.h"

/* =============================================================================
 * Types
 * =============================================================================
 */

/**
 * @enum ra_sci_lin_limits_t
 * @brief Public range limits for the LIN frame identifier.
 *
 * @details The LIN protected identifier carries a 6-bit frame id, so the
 * largest legal value passed to ``ra_sci_lin_send_header`` is 0x3F (63).
 */
typedef enum : uint8_t {
  k_ra_sci_lin_id_max = 0x3FU, /**< Highest legal 6-bit LIN frame id. */
} ra_sci_lin_limits_t;

/**
 * @enum ra_sci_lin_timer_clk_t
 * @brief Break-field timer clock divider (maps to SCI_B XCR0.TCSS[1:0]).
 *
 * @details Selects the divider applied to the Simple-LIN module timer
 * clock (TCLK) that times the break field. The enumerator values equal
 * the on-chip XCR0.TCSS encodings (HUM Ch 38.2.14, p 2221), so no
 * translation table is needed.
 */
typedef enum : uint8_t {
  k_ra_sci_lin_clk_div4  = 1U, /**< Break timer clock = TCLK / 4.  */
  k_ra_sci_lin_clk_div16 = 2U, /**< Break timer clock = TCLK / 16. */
  k_ra_sci_lin_clk_div64 = 3U, /**< Break timer clock = TCLK / 64. */
} ra_sci_lin_timer_clk_t;

/**
 * @enum ra_sci_lin_checksum_mode_t
 * @brief Selector for the classic vs. enhanced LIN checksum.
 *
 * @details Classic (LIN 1.x) sums only the data bytes; enhanced (LIN 2.x)
 * folds the protected identifier into the sum as well. Both then take the
 * inverted modulo-255 sum.
 */
typedef enum : uint8_t {
  k_ra_sci_lin_checksum_classic  = 0U, /**< LIN 1.x: sum of data bytes only.  */
  k_ra_sci_lin_checksum_enhanced = 1U, /**< LIN 2.x: sum of PID + data bytes. */
} ra_sci_lin_checksum_mode_t;

/**
 * @struct ra_sci_lin_cfg_t
 * @brief Configuration descriptor for ``ra_sci_lin_init``.
 *
 * @details
 * Wraps the base async-UART configuration (baud / framing) used to bring
 * the channel up before the mode switch, plus the two Simple-LIN-specific
 * parameters: the break-field timer clock divider and the break-field
 * length register value.
 *
 * cppcheck cannot see tests/ so it flags every field as unused; each
 * member is read in ``ra_sci_lin_init`` in
 * ``libs/ra_hal/src/ra_sci_lin.c``.
 *
 * @invariant ``break_field_len`` <= ``k_ra_sci_xcr2_bflw_max`` (0xFFFE).
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  ra_sci_cfg_t           uart;            /**< Base async-UART baud + framing. */
  ra_sci_lin_timer_clk_t timer_clk;       /**< Break-field timer clock (TCSS). */
  uint16_t               break_field_len; /**< XCR2.BFLW: dominant time =
                                               (break_field_len + 1) x timer
                                               clock. >= 13 bit-times per the
                                               LIN standard; 0xFFFF prohibited. */
} ra_sci_lin_cfg_t;
/* cppcheck-suppress-end [unusedStructMember] */

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Configure an SCI_B channel as a LIN commander node.
 *
 * @details
 * First brings the channel up as an async UART (``ra_sci_init`` with
 * ``cfg->uart``: MSTP gate, baud, framing, TE/RE). It then drops CCR0,
 * switches CCR3.MOD to Simple LIN (110b) while preserving the framing bits
 * the base init programmed, programs the break-field timer clock
 * (XCR0.TCSS) and break-field enable (XCR0.BFE), sets the break-field
 * length (XCR2.BFLW), clears XCR1, and re-enables CCR0.TE + CCR0.RE.
 *
 * @param[in] channel SCI channel number (0..9).
 * @param[in] cfg     Non-NULL LIN configuration descriptor.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok               Channel configured for LIN commander mode.
 * @retval k_ra_err_null_ptr     ``cfg`` was NULL or ``channel`` out of range.
 * @retval k_ra_err_invalid_arg  ``timer_clk`` not a defined divider, or
 *                               ``break_field_len`` exceeds 0xFFFE.
 * @retval k_ra_err_hw_init_failed The underlying ``ra_sci_init`` failed.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre ``ra_mstp_init`` has been called.
 * @post On success, CCR3.MOD == Simple LIN and the break-field timer is
 *       programmed from ``cfg``.
 * @post On success, CCR0.TE and CCR0.RE are set.
 *
 * @note Thread safety: not thread-safe.
 * @see ra_sci_lin_send_header
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sci_lin_init(uint8_t channel, const ra_sci_lin_cfg_t* cfg);

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
 * (``RA_SIMULATOR_MODE``) the timer drain is not modelled, so the wait
 * returns success immediately.
 *
 * @param[in] channel SCI channel number (0..9).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok            Break field emitted (or simulator stub).
 * @retval k_ra_err_null_ptr  ``channel`` out of range.
 * @retval k_ra_err_hw_timeout TCST did not self-clear within the budget.
 *
 * @pre Channel previously configured via ``ra_sci_lin_init``.
 * @pre IRQs masked or single-threaded transmit context.
 * @post On success, the break-field timer is idle (XCR1.TCST clear).
 * @post No data byte has been pushed to TDR by this call.
 *
 * @note Thread safety: not thread-safe.
 * @see ra_sci_lin_send_header
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sci_lin_send_break(uint8_t channel);

/**
 * @brief Transmit a full LIN header: break + SYNC (0x55) + PID.
 *
 * @details
 * Emits the break field (``ra_sci_lin_send_break``), then sends the SYNC
 * byte 0x55 and the protected identifier as two ordinary UART frames via
 * ``ra_sci_putc_polling``. The PID is computed from ``id`` with
 * ``ra_sci_lin_pid`` (6-bit id plus the two LIN parity bits).
 *
 * @param[in] channel SCI channel number (0..9).
 * @param[in] id      6-bit LIN frame identifier (0..63).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok              Header transmitted.
 * @retval k_ra_err_null_ptr    ``channel`` out of range.
 * @retval k_ra_err_invalid_arg ``id`` exceeds 63.
 * @retval k_ra_err_hw_timeout  A TDRE poll or the break wait timed out.
 *
 * @pre Channel previously configured via ``ra_sci_lin_init``.
 * @pre ``id`` <= ``k_ra_sci_lin_id_max``.
 * @post On success, the break field, SYNC, and PID have been clocked out.
 * @post TDR holds the protected identifier (the final byte written).
 *
 * @note Thread safety: not thread-safe.
 * @see ra_sci_lin_pid
 * @see ra_sci_lin_send_break
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sci_lin_send_header(uint8_t channel, uint8_t id);

/* =============================================================================
 * Pure helpers
 * =============================================================================
 */

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
 * @see ra_sci_lin_send_header
 * @since 0.1.0
 */
[[nodiscard]] uint8_t ra_sci_lin_pid(uint8_t id);

/**
 * @brief Compute a classic or enhanced LIN checksum over a data field.
 *
 * @details
 * Sums the data bytes (and, for ``k_ra_sci_lin_checksum_enhanced``, the
 * protected identifier ``pid``) into an accumulator, folds the carry bits
 * back into the low byte to form the modulo-255 sum, and returns the
 * one's complement of that sum. Pure: touches no hardware register.
 *
 * @param[in]  mode         Classic (data only) or enhanced (PID + data).
 * @param[in]  pid          Protected identifier, folded in only when
 *                          ``mode == k_ra_sci_lin_checksum_enhanced``.
 * @param[in]  data         Data-field byte buffer; may be NULL only when
 *                          ``len == 0``.
 * @param[in]  len          Number of data bytes (0..8 for standard LIN).
 * @param[out] out_checksum Receives the 8-bit checksum on success.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok              Checksum computed.
 * @retval k_ra_err_null_ptr    ``out_checksum`` is NULL, or ``data`` is NULL
 *                              while ``len`` > 0.
 * @retval k_ra_err_invalid_arg ``mode`` is not a defined checksum mode.
 *
 * @pre ``out_checksum`` is non-NULL.
 * @pre ``data`` is non-NULL whenever ``len`` > 0.
 * @post On success, ``*out_checksum`` holds the inverted modulo-255 sum.
 * @post No input buffer is modified.
 *
 * @note Thread safety: pure function; safe to call concurrently.
 * @see ra_sci_lin_pid
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sci_lin_checksum(ra_sci_lin_checksum_mode_t mode,
                                           uint8_t                    pid,
                                           const uint8_t*             data,
                                           uint8_t                    len,
                                           uint8_t*                   out_checksum);

#ifdef __cplusplus
}
#endif
