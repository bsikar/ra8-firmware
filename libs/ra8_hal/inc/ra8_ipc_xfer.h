/**
 * @file ra8_ipc_xfer.h
 * @brief Inter-Processor Communication (IPC) HAL driver -- transfer API
 * @ingroup grp_hal_system
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Lifecycle, channel-pair convention helpers, send / receive (single and
 * burst), status / error inspection, and security / privilege attribution
 * prototypes for the RA8D2 IPC mailbox driver. Split out of ``ra8_ipc.h``
 * so each header stays within the repository file-size budget; ``ra8_ipc.h``
 * re-includes this header so existing consumers are unaffected.
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

#include "ra8_err.h"
#include "ra8_ipc_regs.h"
#include "ra8_ipc_types.h"

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Initialise one IPC channel.
 *
 * @details
 * Issues an optional FIFO reset and an optional status clear, then
 * stores the per-channel event mask the dispatch path uses to filter
 * which status bits trigger the callback.
 *
 * The peripheral itself does not have a dedicated MSTPCR bit on the
 * RA8D2 -- IPC sits on the always-on CPU bus and is reachable as soon
 * as the secondary core comes out of reset, so no ``ra8_mstp_enable``
 * call is required.
 *
 * @par State Machine
 * @dot
 * digraph ra8_ipc_xfer_states {
 *   bgcolor="transparent";
 *   rankdir=LR;
 *   node [shape=box, style="rounded,filled", fontname="Helvetica", fontsize=10,
 *         fillcolor="#e8eef7", color="#5a7ca6"];
 *   edge [fontname="Helvetica", fontsize=9, color="#5a7ca6"];
 *
 *   __start [shape=circle, width=0.18, label="", fillcolor="#5a7ca6", color="#5a7ca6"];
 *
 *   Closed [label="Closed"];
 *   Open [label="Open"];
 *
 *   __start -> Closed;
 *   Closed -> Open [label="ra8_ipc_init"];
 *   Open -> Open [label="ra8_ipc_set_event_mask"];
 *   Open -> Open [label="ra8_ipc_reset_fifo"];
 *   Open -> Closed [label="ra8_ipc_deinit"];
 * }
 * @enddot
 *
 * @param[in] cfg Non-NULL configuration descriptor.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                 Channel ready for use.
 * @retval k_ra8_err_null_ptr       ``cfg`` was NULL.
 * @retval k_ra8_err_invalid_arg    ``cfg->channel >= 4``.
 *
 * @pre Caller is in single-threaded init context (no other code is
 *      touching the same channel concurrently).
 * @pre The CPSCU register window is mapped (it always is on Cortex-M85
 *      bring-up).
 * @post Stored event mask matches ``cfg->event_mask``.
 * @post If ``cfg->reset_fifo``, the channel FIFO has been drained.
 *
 * @note Thread safety: not thread-safe.
 *
 * @see ra8_ipc_deinit
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ipc_init(const ra8_ipc_config_t* cfg);

/**
 * @brief Tear down one IPC channel.
 *
 * @details
 * Clears the channel STA register (cancels every IRQn / RDY / FULL /
 * RERR / FERR bit) and resets the FIFO via CLR.RST so the next
 * ``ra8_ipc_init`` starts from a known state. Also drops every per-event
 * line callback registered via ``ra8_ipc_attach_event_handler``.
 *
 * @param[in] channel Channel id 0..3.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Channel cleared.
 * @retval k_ra8_err_invalid_arg ``channel >= 4``.
 *
 * @pre ``ra8_ipc_init`` was previously called for this channel.
 * @pre Caller is in single-threaded shutdown context.
 * @post Channel STA reads zero on a real device.
 * @post FIFO is empty (RDY = 0, FULL = 0).
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_ipc_init
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ipc_deinit(uint8_t channel);

/**
 * @brief Issue a CLR.RST on one channel without altering callbacks.
 *
 * @details
 * Convenience wrapper that drops the FIFO contents (HUM Ch 3.2.14
 * "RST bit" p 217). Useful for application-level resync without
 * having to tear down callbacks via ``ra8_ipc_deinit``.
 *
 * @param[in] channel Channel id 0..3.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              FIFO reset issued.
 * @retval k_ra8_err_invalid_arg ``channel >= 4``.
 *
 * @pre ``ra8_ipc_init`` previously enabled the channel.
 * @pre Caller holds whatever lock co-ordinates this channel.
 * @post STA.RDY and STA.FULL read 0.
 * @post Pending FIFO words are discarded.
 *
 * @note Thread safety: not thread-safe per channel.
 * @see ra8_ipc_deinit
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ipc_reset_fifo(uint8_t channel);

/**
 * @brief Update the event-mask filter for an already-initialized channel.
 *
 * @param[in] channel Channel id 0..3.
 * @param[in] mask    New ``ra8_ipc_event_t`` bitmask.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Mask stored.
 * @retval k_ra8_err_invalid_arg ``channel >= 4``.
 *
 * @pre ``ra8_ipc_init`` previously enabled the channel.
 * @pre ``mask`` only contains bits drawn from ``ra8_ipc_event_t``.
 * @post Subsequent ``ra8_ipc_dispatch`` calls filter against the new mask.
 * @post Stored channel state otherwise unchanged.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_ipc_init
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ipc_set_event_mask(uint8_t channel, uint32_t mask);

/* =============================================================================
 * Channel-pair convention helpers
 * =============================================================================
 */

/**
 * @brief Pick the channel id this core writes to.
 *
 * @details
 * Encodes the channel-pair convention from HUM Ch 3.1 p 204:
 * CPU0 (M85) sends through IPC1 (channels 2/3); CPU1 (M33) sends
 * through IPC0 (channels 0/1). The ``pair`` argument selects between
 * the two channels owned by each unit.
 *
 * @param[in]  core     Local CPU id.
 * @param[in]  pair     Channel pair within the unit (0 -> *_0, 1 -> *_1).
 * @param[out] out_channel Receives the resolved channel id 0..3.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Resolved successfully.
 * @retval k_ra8_err_null_ptr    ``out_channel`` was NULL.
 * @retval k_ra8_err_invalid_arg ``core`` or ``pair`` out of range.
 *
 * @pre ``out_channel`` non-NULL.
 * @pre ``core`` in {k_ra8_ipc_core_cpu0, k_ra8_ipc_core_cpu1}.
 * @post On success, ``*out_channel`` < 4.
 * @post Hardware state untouched.
 *
 * @note Thread safety: re-entrant (pure computation).
 * @see ra8_ipc_channel_for_recv
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_ipc_channel_for_send(ra8_ipc_core_id_t core, uint8_t pair, uint8_t* out_channel);

/**
 * @brief Pick the channel id this core reads from.
 *
 * @details
 * Mirror of ``ra8_ipc_channel_for_send``: CPU0 receives through IPC0
 * (channels 0/1); CPU1 receives through IPC1 (channels 2/3).
 *
 * @param[in]  core         Local CPU id.
 * @param[in]  pair         Pair within the unit (0 -> *_0, 1 -> *_1).
 * @param[out] out_channel  Receives the resolved channel id 0..3.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Resolved successfully.
 * @retval k_ra8_err_null_ptr    ``out_channel`` was NULL.
 * @retval k_ra8_err_invalid_arg ``core`` or ``pair`` out of range.
 *
 * @pre ``out_channel`` non-NULL.
 * @pre ``core`` in {k_ra8_ipc_core_cpu0, k_ra8_ipc_core_cpu1}.
 * @post On success, ``*out_channel`` < 4.
 * @post Hardware state untouched.
 *
 * @note Thread safety: re-entrant (pure computation).
 * @see ra8_ipc_channel_for_send
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_ipc_channel_for_recv(ra8_ipc_core_id_t core, uint8_t pair, uint8_t* out_channel);

/* =============================================================================
 * Send / receive
 * =============================================================================
 */

/**
 * @brief Generate a maskable IRQ event on the peer core.
 *
 * @param[in] channel  Channel id 0..3 (call from the *send* side).
 * @param[in] event_id IRQ event line 0..7.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Event issued.
 * @retval k_ra8_err_invalid_arg ``channel >= 4`` or ``event_id > 7``.
 *
 * @pre ``ra8_ipc_init`` has been called for this channel.
 * @pre Receiving core has unmasked the IPCnIRQm vector in NVIC.
 * @post Peer STA.IRQn becomes 1 (until peer writes CLR.CLRn).
 * @post Local IPC ISET write has been issued exactly once.
 *
 * @note Thread safety: re-entrant per channel; no global state.
 *
 * @par Example
 * @code
 * (void)ra8_ipc_send_event(2, k_ra8_ipc_irq_event_0);
 * @endcode
 *
 * @see ra8_ipc_clear_event
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ipc_send_event(uint8_t channel, ra8_ipc_irq_event_id_t event_id);

/**
 * @brief Clear one received IRQ event line on the local core.
 *
 * @param[in] channel  Channel id 0..3 (call from the *receive* side).
 * @param[in] event_id IRQ event line 0..7 to acknowledge.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Bit cleared.
 * @retval k_ra8_err_invalid_arg ``channel >= 4`` or ``event_id > 7``.
 *
 * @pre This core observed the IPCnIRQm interrupt for ``channel``.
 * @pre ``ra8_ipc_init`` previously enabled the channel.
 * @post STA.IRQn reads 0 on the next register fetch.
 * @post No pending CLR write remains for this event line.
 *
 * @note Thread safety: re-entrant per channel.
 * @see ra8_ipc_send_event
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ipc_clear_event(uint8_t channel, ra8_ipc_irq_event_id_t event_id);

/**
 * @brief Push one 32-bit word into the channel TX FIFO.
 *
 * @details
 * Writes ``message`` to the channel TXD register. The peripheral sets
 * STA.RDY on the receiving side. If the FIFO was full at write time
 * the write is silently dropped and STA.FERR is set (HUM Ch 3.2.12
 * p 215). The driver returns ``k_ra8_err_busy`` in that case so the
 * caller can either retry or escalate.
 *
 * @param[in] channel Channel id 0..3.
 * @param[in] message 32-bit payload to enqueue.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Word accepted.
 * @retval k_ra8_err_invalid_arg ``channel >= 4``.
 * @retval k_ra8_err_busy        FIFO full at write time.
 *
 * @pre ``ra8_ipc_init`` previously enabled the channel.
 * @pre Caller holds whatever lock co-ordinates this channel.
 * @post On success, STA.RDY is set on the peer.
 * @post On busy, STA.FERR is set on the local core.
 *
 * @note Thread safety: not thread-safe per channel.
 * @see ra8_ipc_recv_message
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ipc_send_message(uint8_t channel, uint32_t message);

/**
 * @brief Bounded-retry variant of ``ra8_ipc_send_message``.
 *
 * @details
 * Polls ``STA.FULL`` up to ``max_retries`` times, clearing FERR at each
 * iteration so a previous overflow does not stick. Returns
 * ``k_ra8_err_hw_timeout`` if the FIFO is still full after the loop.
 *
 * @param[in] channel     Channel id 0..3.
 * @param[in] message     32-bit payload to enqueue.
 * @param[in] max_retries Iteration cap (clamped to ``k_ra8_ipc_retry_max``).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               Word accepted.
 * @retval k_ra8_err_invalid_arg  ``channel >= 4``.
 * @retval k_ra8_err_hw_timeout   FIFO full for the full retry budget.
 *
 * @pre ``ra8_ipc_init`` previously enabled the channel.
 * @pre ``max_retries`` <= ``k_ra8_ipc_retry_max``.
 * @post On success, STA.RDY is set on the peer.
 * @post On timeout, STA.FERR has been cleared at least once.
 *
 * @note Thread safety: not thread-safe per channel.
 * @see ra8_ipc_send_message
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_ipc_send_message_retry(uint8_t channel, uint32_t message, uint16_t max_retries);

/**
 * @brief Push up to ``count`` words into the channel TX FIFO.
 *
 * @details
 * Writes one word per non-full FIFO stage; honours the 4-stage depth
 * documented in HUM Ch 3.1 p 204. Stops as soon as STA.FULL is set --
 * does not spin or retry. ``out_written`` returns the number of words
 * actually pushed; the caller is expected to back-pressure on the
 * remainder.
 *
 * @param[in]  channel     Channel id 0..3.
 * @param[in]  data        Source array, ``count`` words long.
 * @param[in]  count       Number of words to attempt to send.
 * @param[out] out_written Receives count actually written (may be 0).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              At least one word written, or zero
 *                               written because FIFO was full.
 * @retval k_ra8_err_invalid_arg ``channel >= 4`` or ``count == 0``.
 * @retval k_ra8_err_null_ptr    ``data`` or ``out_written`` was NULL.
 *
 * @pre ``ra8_ipc_init`` previously enabled the channel.
 * @pre ``data`` non-NULL and ``out_written`` non-NULL.
 * @post ``*out_written`` <= ``count``.
 * @post FIFO depth never exceeds ``k_ra8_ipc_fifo_depth``.
 *
 * @note Thread safety: not thread-safe per channel.
 * @see ra8_ipc_send_message
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_ipc_send_burst(uint8_t channel, const uint32_t* data, uint32_t count, uint32_t* out_written);

/**
 * @brief Pop one 32-bit word from the channel RX FIFO.
 *
 * @details
 * Reads the channel RXD register. If RDY was 0 at read time the read
 * returns zero and STA.RERR is set (HUM Ch 3.2.13 p 216). The driver
 * returns ``k_ra8_err_no_data`` in that case so the caller can retry
 * once a peer has written.
 *
 * @param[in]  channel Channel id 0..3.
 * @param[out] out_msg Receives the next FIFO word; left untouched on
 *                     error.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Word read.
 * @retval k_ra8_err_invalid_arg ``channel >= 4``.
 * @retval k_ra8_err_null_ptr    ``out_msg`` was NULL.
 * @retval k_ra8_err_no_data     FIFO empty at read time.
 *
 * @pre ``ra8_ipc_init`` previously enabled the channel.
 * @pre Caller holds whatever lock co-ordinates this channel.
 * @post On success, FIFO advances by one word.
 * @post On empty, STA.RERR is set on the local core.
 *
 * @note Thread safety: not thread-safe per channel.
 * @see ra8_ipc_send_message
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ipc_recv_message(uint8_t channel, uint32_t* out_msg);

/**
 * @brief Bounded-retry variant of ``ra8_ipc_recv_message``.
 *
 * @details
 * Polls STA.RDY up to ``max_retries`` times, clearing RERR at each
 * iteration. Returns ``k_ra8_err_hw_timeout`` if no word arrived in
 * the budget.
 *
 * @param[in]  channel     Channel id 0..3.
 * @param[out] out_msg     Receives the popped word on success.
 * @param[in]  max_retries Iteration cap (<= ``k_ra8_ipc_retry_max``).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               Word read.
 * @retval k_ra8_err_invalid_arg  Bad channel or NULL output pointer.
 * @retval k_ra8_err_hw_timeout   No word arrived in the retry budget.
 *
 * @pre ``ra8_ipc_init`` previously enabled the channel.
 * @pre ``out_msg`` non-NULL.
 * @post On success, ``*out_msg`` reflects RXD.
 * @post On timeout, STA.RERR has been cleared at least once.
 *
 * @note Thread safety: not thread-safe per channel.
 * @see ra8_ipc_recv_message
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_ipc_recv_message_retry(uint8_t channel, uint32_t* out_msg, uint16_t max_retries);

/**
 * @brief Drain up to ``capacity`` words from the channel RX FIFO.
 *
 * @details
 * Pops one word per RDY-set FIFO stage; stops as soon as RDY drops or
 * the buffer fills. ``out_read`` returns how many words landed in
 * ``out_data``.
 *
 * @param[in]  channel  Channel id 0..3.
 * @param[out] out_data Destination array, at least ``capacity`` words.
 * @param[in]  capacity Maximum number of words to read.
 * @param[out] out_read Receives count actually popped (may be 0).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               Drain succeeded (may have read 0).
 * @retval k_ra8_err_invalid_arg  ``channel >= 4`` or ``capacity == 0``.
 * @retval k_ra8_err_null_ptr     ``out_data`` or ``out_read`` was NULL.
 *
 * @pre ``ra8_ipc_init`` previously enabled the channel.
 * @pre ``out_data`` non-NULL and ``out_read`` non-NULL.
 * @post ``*out_read`` <= ``capacity``.
 * @post After the call STA.RDY may still be 1 if the peer kept writing.
 *
 * @note Thread safety: not thread-safe per channel.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_ipc_recv_burst(uint8_t channel, uint32_t* out_data, uint32_t capacity, uint32_t* out_read);

/* =============================================================================
 * Status / errors
 * =============================================================================
 */

/**
 * @brief Read the raw channel status register.
 *
 * @param[in]  channel Channel id 0..3.
 * @param[out] out_sta Receives STA value (full 32-bit raw register).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Status read.
 * @retval k_ra8_err_invalid_arg ``channel >= 4``.
 * @retval k_ra8_err_null_ptr    ``out_sta`` was NULL.
 *
 * @pre Channel was initialized.
 * @pre ``out_sta`` non-NULL.
 * @post ``*out_sta`` reflects the live STA value.
 * @post No registers are mutated.
 *
 * @note Thread safety: re-entrant; pure read.
 * @see ra8_ipc_clear_status
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ipc_get_status(uint8_t channel, uint32_t* out_sta);

/**
 * @brief Clear selected status bits on a channel.
 *
 * @param[in] channel Channel id 0..3.
 * @param[in] mask    Bitmask of ``ra8_ipc_event_t`` flags to clear.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Mask written.
 * @retval k_ra8_err_invalid_arg ``channel >= 4``.
 *
 * @pre Channel was initialized.
 * @pre ``mask`` only contains bits drawn from ``ra8_ipc_event_t``.
 * @post Bits requested in ``mask`` read back as 0 on next get_status.
 * @post No other STA bits are disturbed.
 *
 * @note Thread safety: re-entrant per channel.
 * @see ra8_ipc_get_status
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ipc_clear_status(uint8_t channel, uint32_t mask);

/**
 * @brief Clear RERR + FERR on the channel.
 *
 * @param[in] channel Channel id 0..3.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Errors cleared.
 * @retval k_ra8_err_invalid_arg ``channel >= 4``.
 *
 * @pre Channel was initialized.
 * @pre Caller observed (or expects) STA.RERR or STA.FERR to be set.
 * @post STA.RERR == 0 and STA.FERR == 0 on next read.
 * @post No other STA bits are disturbed.
 *
 * @note Thread safety: re-entrant per channel.
 * @see ra8_ipc_clear_status
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ipc_clear_errors(uint8_t channel);

/**
 * @brief Predicate: would ``ra8_ipc_send_message`` succeed right now?
 *
 * @param[in]  channel    Channel id 0..3.
 * @param[out] out_can_send Receives true if FIFO has at least one free slot.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Predicate evaluated.
 * @retval k_ra8_err_invalid_arg ``channel >= 4``.
 * @retval k_ra8_err_null_ptr    ``out_can_send`` was NULL.
 *
 * @pre Channel was initialized.
 * @pre ``out_can_send`` non-NULL.
 * @post No registers are mutated.
 * @post ``*out_can_send`` reflects ``!STA.FULL``.
 *
 * @note Thread safety: re-entrant; pure read.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ipc_can_send(uint8_t channel, bool* out_can_send);

/**
 * @brief Predicate: would ``ra8_ipc_recv_message`` succeed right now?
 *
 * @param[in]  channel      Channel id 0..3.
 * @param[out] out_has_data Receives true if at least one word is queued.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Predicate evaluated.
 * @retval k_ra8_err_invalid_arg ``channel >= 4``.
 * @retval k_ra8_err_null_ptr    ``out_has_data`` was NULL.
 *
 * @pre Channel was initialized.
 * @pre ``out_has_data`` non-NULL.
 * @post No registers are mutated.
 * @post ``*out_has_data`` reflects ``STA.RDY``.
 *
 * @note Thread safety: re-entrant; pure read.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ipc_has_data(uint8_t channel, bool* out_has_data);

/* =============================================================================
 * Security / privilege attribution
 * =============================================================================
 */

/**
 * @brief Read the security / privilege attribution for one channel.
 *
 * @details
 * Decodes the matching SAIPCIRn / PAIPCIRn bits in IPCSAR / IPCPAR
 * (HUM Ch 3.2.1 p 207, Ch 3.2.2 p 208). Useful for the secondary
 * core to confirm it has access to the channel before issuing a
 * register write that the SAU / IDAU would silently drop.
 *
 * @param[in]  channel  Channel id 0..3.
 * @param[out] out_attr Receives ``secure`` and ``privileged`` flags.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Attribution read.
 * @retval k_ra8_err_invalid_arg ``channel >= 4``.
 * @retval k_ra8_err_null_ptr    ``out_attr`` was NULL.
 *
 * @pre IPCSAR / IPCPAR are mapped (CPSCU window).
 * @pre ``out_attr`` non-NULL.
 * @post ``out_attr->secure`` reflects SAIPCIRn (1 = non-secure).
 * @post ``out_attr->privileged`` reflects PAIPCIRn (1 = unprivileged).
 *
 * @note Thread safety: re-entrant; pure read.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ipc_get_attribution(uint8_t channel, ra8_ipc_attr_t* out_attr);

/**
 * @brief Read the attribution for one NMI unit.
 *
 * @details
 * IPCSAR.SAIPCNMI[0..1] and IPCPAR.PAIPCNMI[0..1] live at bits 8/9 of
 * their respective registers (HUM Ch 3.2.1 p 205-207). Provided so the
 * NMI-injection code can pre-flight before issuing IPCnNMISET writes.
 *
 * @param[in]  unit     NMI unit id 0..1 (k_ra8_ipc_unit_ipc0 / ipc1).
 * @param[out] out_attr Receives ``secure`` / ``privileged`` flags.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Attribution read.
 * @retval k_ra8_err_invalid_arg ``unit >= 2``.
 * @retval k_ra8_err_null_ptr    ``out_attr`` was NULL.
 *
 * @pre ``out_attr`` non-NULL.
 * @pre IPCSAR / IPCPAR are mapped.
 * @post Output reflects SAIPCNMIu / PAIPCNMIu.
 * @post No registers are mutated.
 *
 * @note Thread safety: re-entrant; pure read.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ipc_get_nmi_attribution(uint8_t unit, ra8_ipc_attr_t* out_attr);

/**
 * @brief Read the attribution for an IPCSEM group.
 *
 * @details
 * Wraps SAIPCSEM[0..1] / PAIPCSEM[0..1]. Group 0 covers IPCSEM0..7;
 * group 1 covers IPCSEM8..15 (HUM Ch 3.3.1 p 228).
 *
 * @param[in]  group    Attribution group.
 * @param[out] out_attr Receives ``secure`` / ``privileged`` flags.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Attribution read.
 * @retval k_ra8_err_invalid_arg ``group`` not in {0, 1}.
 * @retval k_ra8_err_null_ptr    ``out_attr`` was NULL.
 *
 * @pre ``out_attr`` non-NULL.
 * @pre IPCSAR / IPCPAR are mapped.
 * @post Output reflects SAIPCSEMg / PAIPCSEMg.
 * @post No registers are mutated.
 *
 * @note Thread safety: re-entrant; pure read.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ipc_get_sem_attribution(ra8_ipc_sem_attr_group_t group,
                                                    ra8_ipc_attr_t*          out_attr);

/**
 * @brief Predicate: does this core's attribution allow writing the channel?
 *
 * @details
 * Reads IPCSAR / IPCPAR for the channel and compares against the
 * core's expected security / privilege state. The driver does not
 * know the running core's state directly -- the caller passes a
 * desired ``required`` attribution and the function returns true if
 * the channel matches.
 *
 * @param[in]  channel       Channel id 0..3.
 * @param[in]  required      Desired attribution.
 * @param[out] out_can_access Receives true on a match.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Predicate evaluated.
 * @retval k_ra8_err_invalid_arg ``channel >= 4``.
 * @retval k_ra8_err_null_ptr    ``out_can_access`` was NULL.
 *
 * @pre ``out_can_access`` non-NULL.
 * @pre Channel attribution registers are accessible.
 * @post ``*out_can_access`` reflects (live attr == ``required``).
 * @post No registers are mutated.
 *
 * @note Thread safety: re-entrant; pure read.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_ipc_can_access(uint8_t channel, ra8_ipc_attr_t const* required, bool* out_can_access);

#ifdef __cplusplus
}
#endif
