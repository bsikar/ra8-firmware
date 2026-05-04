/**
 * @file ra_ipc.h
 * @brief Inter-Processor Communication (IPC) HAL driver -- public API
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Bare-metal driver for the RA8D2 IPC mailbox between the Cortex-M85
 * (CPU0) and Cortex-M33 (CPU1). Exposes the full HUM Ch 3 surface:
 *
 * - **Lifecycle**: ``ra_ipc_init`` / ``ra_ipc_deinit`` / ``ra_ipc_reset_fifo``
 *   / ``ra_ipc_set_event_mask`` per FIFO channel.
 * - **Channel pairing convention**: ``ra_ipc_channel_for_send`` /
 *   ``ra_ipc_channel_for_recv`` map the local CPU id (CPU0 / CPU1) to the
 *   right IPC unit so application code does not hard-code channel ids.
 * - **Per-channel attribution check**: ``ra_ipc_can_access`` reads
 *   IPCSAR / IPCPAR before issuing a register write that the SAU /
 *   IDAU would otherwise silently drop.
 * - **Send / receive**: single-shot ``ra_ipc_send_message`` /
 *   ``ra_ipc_recv_message`` plus burst variants
 *   ``ra_ipc_send_burst`` / ``ra_ipc_recv_burst`` with overflow
 *   protection driven by the 4-stage FIFO depth (HUM Ch 3.1 p 204).
 * - **Maskable IRQ events**: ``ra_ipc_send_event`` /
 *   ``ra_ipc_clear_event`` plus per-event-line callback attach via
 *   ``ra_ipc_attach_event_handler``. Each of the eight IRQ lines
 *   per channel can have its own decoded handler.
 * - **NMI surface**: ``ra_ipc_nmi_send`` / ``ra_ipc_nmi_clear`` /
 *   ``ra_ipc_nmi_get_status`` / ``ra_ipc_attach_nmi_handler`` /
 *   ``ra_ipc_dispatch_nmi`` cover IPC0NMI* and IPC1NMI*.
 * - **Hardware semaphores**: ``ra_ipc_sem_try_take`` /
 *   ``ra_ipc_sem_release`` / ``ra_ipc_sem_take_timeout`` /
 *   ``ra_ipc_sem_is_locked`` wrap IPCSEM0..15 with the test-and-set
 *   semantics documented in HUM Ch 3.2.3 p 210.
 * - **Real ISR wiring**: ``ra_ipc_install_isr`` registers the IPC
 *   IRQ event with ``ra_isr_register`` so production builds get a
 *   real interrupt path. Tests can still call ``ra_ipc_dispatch``
 *   directly to drive the dispatch logic.
 * - **Shared-memory ring buffer**: ``ra_ipc_ring_*`` builds a
 *   producer/consumer ring on top of an SRAM region; the IPC FIFO
 *   word carries the producer head index.
 * - **Error recovery**: ``ra_ipc_send_message_retry`` /
 *   ``ra_ipc_recv_message_retry`` clear FERR/RERR and retry up to a
 *   bounded number of times. ``ra_ipc_clear_errors`` clears RERR /
 *   FERR explicitly.
 *
 * Channel numbering (matches ``ra8d2_ipc_regs.h``):
 *
 * | id | direction        | unit | FIFO  |
 * |---:|------------------|------|-------|
 * |  0 | CPU1 -> CPU0     | IPC0 | FIFO00|
 * |  1 | CPU1 -> CPU0     | IPC0 | FIFO01|
 * |  2 | CPU0 -> CPU1     | IPC1 | FIFO10|
 * |  3 | CPU0 -> CPU1     | IPC1 | FIFO11|
 *
 * The two cores typically pair channels: e.g. M85 sends through
 * channel 2 and reads replies on channel 0. The
 * ``ra_ipc_channel_for_*`` helpers encode that convention so user
 * code remains symmetric across both cores.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8d2_ipc_regs.h"
#include "ra_err.h"

/* =============================================================================
 * Public types
 * =============================================================================
 */

/**
 * @enum ra_ipc_event_t
 * @brief Bitmask reported to the user callback during dispatch.
 *
 * @details
 * Mirrors the IPCnSTAm bit layout from HUM Ch 3.2.10 p 214 in a form
 * suitable for the public API. Multiple flags can coexist in a single
 * dispatch -- e.g. RDY + IRQ0 if a peer wrote a message and signalled
 * an event in the same window.
 */
typedef enum : uint32_t {
  k_ra_ipc_event_none      = 0x00000000U, /**< No event pending.            */
  k_ra_ipc_event_irq0      = 0x00000001U, /**< Maskable IRQ event line 0.   */
  k_ra_ipc_event_irq1      = 0x00000002U, /**< Maskable IRQ event line 1.   */
  k_ra_ipc_event_irq2      = 0x00000004U, /**< Maskable IRQ event line 2.   */
  k_ra_ipc_event_irq3      = 0x00000008U, /**< Maskable IRQ event line 3.   */
  k_ra_ipc_event_irq4      = 0x00000010U, /**< Maskable IRQ event line 4.   */
  k_ra_ipc_event_irq5      = 0x00000020U, /**< Maskable IRQ event line 5.   */
  k_ra_ipc_event_irq6      = 0x00000040U, /**< Maskable IRQ event line 6.   */
  k_ra_ipc_event_irq7      = 0x00000080U, /**< Maskable IRQ event line 7.   */
  k_ra_ipc_event_irq_all   = 0x000000FFU, /**< IRQ7..IRQ0 union mask.       */
  k_ra_ipc_event_msg_ready = 0x00010000U, /**< RDY: receive FIFO non-empty. */
  k_ra_ipc_event_fifo_full = 0x00020000U, /**< FULL: send FIFO is full.     */
  k_ra_ipc_event_err_empty = 0x01000000U, /**< RERR: read-while-empty.      */
  k_ra_ipc_event_err_full  = 0x02000000U, /**< FERR: write-while-full.      */
} ra_ipc_event_t;

/**
 * @enum ra_ipc_irq_event_id_t
 * @brief Index of an IRQ event line within a channel (0..7).
 */
typedef enum : uint8_t {
  k_ra_ipc_irq_event_0 = 0U,
  k_ra_ipc_irq_event_1 = 1U,
  k_ra_ipc_irq_event_2 = 2U,
  k_ra_ipc_irq_event_3 = 3U,
  k_ra_ipc_irq_event_4 = 4U,
  k_ra_ipc_irq_event_5 = 5U,
  k_ra_ipc_irq_event_6 = 6U,
  k_ra_ipc_irq_event_7 = 7U,
} ra_ipc_irq_event_id_t;

/**
 * @enum ra_ipc_retry_limits_t
 * @brief Retry-loop bounds used by the bounded-wait helpers.
 *
 * @details
 * NASA Power of 10 Rule 2 demands all loops have statically-provable
 * upper bounds. ``ra_ipc_send_message_retry`` and
 * ``ra_ipc_recv_message_retry`` use these enums so the loop bound is
 * a compile-time constant, not a magic number.
 */
typedef enum : uint16_t {
  k_ra_ipc_retry_default = 16U,   /**< Default poll bound for FIFO retry. */
  k_ra_ipc_retry_max     = 1024U, /**< Hard cap accepted by the API.      */
  k_ra_ipc_sem_take_max  = 1024U, /**< Hard cap on semaphore-take spins.  */
} ra_ipc_retry_limits_t;

/**
 * @struct ra_ipc_attr_t
 * @brief Snapshot of the security / privilege attribution for one
 *        IPC channel, decoded from IPCSAR / IPCPAR.
 *
 * @details
 * cppcheck cannot see tests/ so it flags every field as unused; each
 * member is written by ``ra_ipc_get_attribution`` in
 * ``libs/ra_hal/src/ra_ipc.c`` and read by the unit tests.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  bool secure;     /**< true -> non-secure, false -> secure (IPCSAR.SAIPCIRn).      */
  bool privileged; /**< true -> unprivileged, false -> privileged (IPCPAR.PAIPCIRn).*/
} ra_ipc_attr_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @struct ra_ipc_config_t
 * @brief Per-channel configuration descriptor passed to
 *        ``ra_ipc_init``.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint8_t  channel;      /**< Channel id 0..3.                          */
  bool     reset_fifo;   /**< true -> issue CLR.RST during init.        */
  bool     clear_status; /**< true -> clear all IRQ + error bits.       */
  uint32_t event_mask;   /**< Bitmask of events the user cares about.   */
} ra_ipc_config_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @typedef ra_ipc_event_fn_t
 * @brief IPC event callback signature.
 *
 * @param[in] ctx        Context pointer captured at attach time.
 * @param[in] channel    Channel id 0..3 that fired the event.
 * @param[in] event_mask Bitwise OR of ``ra_ipc_event_t`` flags.
 * @param[in] message    Message word read from RXD when the event
 *                       set ``k_ra_ipc_event_msg_ready``; 0 otherwise.
 */
typedef void (*ra_ipc_event_fn_t)(void*    ctx,
                                  uint8_t  channel,
                                  uint32_t event_mask,
                                  uint32_t message);

/**
 * @typedef ra_ipc_irq_fn_t
 * @brief Per-IRQ-event-line callback signature.
 *
 * @param[in] ctx     Context pointer captured at attach time.
 * @param[in] channel Channel id 0..3.
 * @param[in] event_id IRQ event id 0..7 that fired.
 */
typedef void (*ra_ipc_irq_fn_t)(void* ctx, uint8_t channel, ra_ipc_irq_event_id_t event_id);

/**
 * @typedef ra_ipc_nmi_fn_t
 * @brief NMI dispatch callback signature.
 *
 * @param[in] ctx  Context pointer captured at attach time.
 * @param[in] unit NMI unit id 0..1 (k_ra_ipc_unit_ipc0 / ipc1).
 */
typedef void (*ra_ipc_nmi_fn_t)(void* ctx, uint8_t unit);

/**
 * @struct ra_ipc_ring_t
 * @brief Shared-memory ring-buffer descriptor used by the producer /
 *        consumer protocol primitives.
 *
 * @details
 * The hardware FIFO carries 32-bit words only -- larger payloads are
 * placed in a shared SRAM region and the FIFO carries the producer
 * head index. The ring uses a single hardware semaphore (IPCSEMn) for
 * mutual exclusion of the head / tail update, and the chosen IPC
 * channel for cross-core notification.
 *
 * cppcheck cannot see the tests so it flags every field as unused.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  volatile uint32_t*    slots;     /**< Backing SRAM array (caller-owned).  */
  volatile uint32_t*    head;      /**< Producer write index (shared).      */
  volatile uint32_t*    tail;      /**< Consumer read index (shared).       */
  uint32_t              capacity;  /**< Number of 32-bit slots in @p slots. */
  uint8_t               channel;   /**< IPC channel used for notification.  */
  uint8_t               sem_id;    /**< IPCSEM index used for exclusion.    */
  ra_ipc_irq_event_id_t notify_id; /**< IRQ event line used for notify.     */
} ra_ipc_ring_t;
/* cppcheck-suppress-end [unusedStructMember] */

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
 * as the secondary core comes out of reset, so no ``ra_mstp_enable``
 * call is required.
 *
 * @par State Machine
 * @startuml
 * [*] --> Closed
 * Closed --> Open : ra_ipc_init
 * Open --> Open : ra_ipc_set_event_mask
 * Open --> Open : ra_ipc_reset_fifo
 * Open --> Closed : ra_ipc_deinit
 * @enduml
 *
 * @param[in] cfg Non-NULL configuration descriptor.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                 Channel ready for use.
 * @retval k_ra_err_null_ptr       ``cfg`` was NULL.
 * @retval k_ra_err_invalid_arg    ``cfg->channel >= 4``.
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
 * @see ra_ipc_deinit
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ipc_init(const ra_ipc_config_t* cfg);

/**
 * @brief Tear down one IPC channel.
 *
 * @details
 * Clears the channel STA register (cancels every IRQn / RDY / FULL /
 * RERR / FERR bit) and resets the FIFO via CLR.RST so the next
 * ``ra_ipc_init`` starts from a known state. Also drops every per-event
 * line callback registered via ``ra_ipc_attach_event_handler``.
 *
 * @param[in] channel Channel id 0..3.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok              Channel cleared.
 * @retval k_ra_err_invalid_arg ``channel >= 4``.
 *
 * @pre ``ra_ipc_init`` was previously called for this channel.
 * @pre Caller is in single-threaded shutdown context.
 * @post Channel STA reads zero on a real device.
 * @post FIFO is empty (RDY = 0, FULL = 0).
 *
 * @note Thread safety: not thread-safe.
 * @see ra_ipc_init
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ipc_deinit(uint8_t channel);

/**
 * @brief Issue a CLR.RST on one channel without altering callbacks.
 *
 * @details
 * Convenience wrapper that drops the FIFO contents (HUM Ch 3.2.14
 * "RST bit" p 217). Useful for application-level resync without
 * having to tear down callbacks via ``ra_ipc_deinit``.
 *
 * @param[in] channel Channel id 0..3.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok              FIFO reset issued.
 * @retval k_ra_err_invalid_arg ``channel >= 4``.
 *
 * @pre ``ra_ipc_init`` previously enabled the channel.
 * @pre Caller holds whatever lock co-ordinates this channel.
 * @post STA.RDY and STA.FULL read 0.
 * @post Pending FIFO words are discarded.
 *
 * @note Thread safety: not thread-safe per channel.
 * @see ra_ipc_deinit
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ipc_reset_fifo(uint8_t channel);

/**
 * @brief Update the event-mask filter for an already-initialised channel.
 *
 * @param[in] channel Channel id 0..3.
 * @param[in] mask    New ``ra_ipc_event_t`` bitmask.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok              Mask stored.
 * @retval k_ra_err_invalid_arg ``channel >= 4``.
 *
 * @pre ``ra_ipc_init`` previously enabled the channel.
 * @pre ``mask`` only contains bits drawn from ``ra_ipc_event_t``.
 * @post Subsequent ``ra_ipc_dispatch`` calls filter against the new mask.
 * @post Stored channel state otherwise unchanged.
 *
 * @note Thread safety: not thread-safe.
 * @see ra_ipc_init
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ipc_set_event_mask(uint8_t channel, uint32_t mask);

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
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok              Resolved successfully.
 * @retval k_ra_err_null_ptr    ``out_channel`` was NULL.
 * @retval k_ra_err_invalid_arg ``core`` or ``pair`` out of range.
 *
 * @pre ``out_channel`` non-NULL.
 * @pre ``core`` in {k_ra_ipc_core_cpu0, k_ra_ipc_core_cpu1}.
 * @post On success, ``*out_channel`` < 4.
 * @post Hardware state untouched.
 *
 * @note Thread safety: re-entrant (pure computation).
 * @see ra_ipc_channel_for_recv
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_ipc_channel_for_send(ra_ipc_core_id_t core, uint8_t pair, uint8_t* out_channel);

/**
 * @brief Pick the channel id this core reads from.
 *
 * @details
 * Mirror of ``ra_ipc_channel_for_send``: CPU0 receives through IPC0
 * (channels 0/1); CPU1 receives through IPC1 (channels 2/3).
 *
 * @param[in]  core         Local CPU id.
 * @param[in]  pair         Pair within the unit (0 -> *_0, 1 -> *_1).
 * @param[out] out_channel  Receives the resolved channel id 0..3.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok              Resolved successfully.
 * @retval k_ra_err_null_ptr    ``out_channel`` was NULL.
 * @retval k_ra_err_invalid_arg ``core`` or ``pair`` out of range.
 *
 * @pre ``out_channel`` non-NULL.
 * @pre ``core`` in {k_ra_ipc_core_cpu0, k_ra_ipc_core_cpu1}.
 * @post On success, ``*out_channel`` < 4.
 * @post Hardware state untouched.
 *
 * @note Thread safety: re-entrant (pure computation).
 * @see ra_ipc_channel_for_send
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_ipc_channel_for_recv(ra_ipc_core_id_t core, uint8_t pair, uint8_t* out_channel);

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
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok              Event issued.
 * @retval k_ra_err_invalid_arg ``channel >= 4`` or ``event_id > 7``.
 *
 * @pre ``ra_ipc_init`` has been called for this channel.
 * @pre Receiving core has unmasked the IPCnIRQm vector in NVIC.
 * @post Peer STA.IRQn becomes 1 (until peer writes CLR.CLRn).
 * @post Local IPC ISET write has been issued exactly once.
 *
 * @note Thread safety: re-entrant per channel; no global state.
 *
 * @par Example
 * @code
 * (void)ra_ipc_send_event(2, k_ra_ipc_irq_event_0);
 * @endcode
 *
 * @see ra_ipc_clear_event
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ipc_send_event(uint8_t channel, ra_ipc_irq_event_id_t event_id);

/**
 * @brief Clear one received IRQ event line on the local core.
 *
 * @param[in] channel  Channel id 0..3 (call from the *receive* side).
 * @param[in] event_id IRQ event line 0..7 to acknowledge.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok              Bit cleared.
 * @retval k_ra_err_invalid_arg ``channel >= 4`` or ``event_id > 7``.
 *
 * @pre This core observed the IPCnIRQm interrupt for ``channel``.
 * @pre ``ra_ipc_init`` previously enabled the channel.
 * @post STA.IRQn reads 0 on the next register fetch.
 * @post No pending CLR write remains for this event line.
 *
 * @note Thread safety: re-entrant per channel.
 * @see ra_ipc_send_event
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ipc_clear_event(uint8_t channel, ra_ipc_irq_event_id_t event_id);

/**
 * @brief Push one 32-bit word into the channel TX FIFO.
 *
 * @details
 * Writes ``message`` to the channel TXD register. The peripheral sets
 * STA.RDY on the receiving side. If the FIFO was full at write time
 * the write is silently dropped and STA.FERR is set (HUM Ch 3.2.12
 * p 215). The driver returns ``k_ra_err_busy`` in that case so the
 * caller can either retry or escalate.
 *
 * @param[in] channel Channel id 0..3.
 * @param[in] message 32-bit payload to enqueue.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok              Word accepted.
 * @retval k_ra_err_invalid_arg ``channel >= 4``.
 * @retval k_ra_err_busy        FIFO full at write time.
 *
 * @pre ``ra_ipc_init`` previously enabled the channel.
 * @pre Caller holds whatever lock co-ordinates this channel.
 * @post On success, STA.RDY is set on the peer.
 * @post On busy, STA.FERR is set on the local core.
 *
 * @note Thread safety: not thread-safe per channel.
 * @see ra_ipc_recv_message
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ipc_send_message(uint8_t channel, uint32_t message);

/**
 * @brief Bounded-retry variant of ``ra_ipc_send_message``.
 *
 * @details
 * Polls ``STA.FULL`` up to ``max_retries`` times, clearing FERR at each
 * iteration so a previous overflow does not stick. Returns
 * ``k_ra_err_hw_timeout`` if the FIFO is still full after the loop.
 *
 * @param[in] channel     Channel id 0..3.
 * @param[in] message     32-bit payload to enqueue.
 * @param[in] max_retries Iteration cap (clamped to ``k_ra_ipc_retry_max``).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok               Word accepted.
 * @retval k_ra_err_invalid_arg  ``channel >= 4``.
 * @retval k_ra_err_hw_timeout   FIFO full for the full retry budget.
 *
 * @pre ``ra_ipc_init`` previously enabled the channel.
 * @pre ``max_retries`` <= ``k_ra_ipc_retry_max``.
 * @post On success, STA.RDY is set on the peer.
 * @post On timeout, STA.FERR has been cleared at least once.
 *
 * @note Thread safety: not thread-safe per channel.
 * @see ra_ipc_send_message
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_ipc_send_message_retry(uint8_t channel, uint32_t message, uint16_t max_retries);

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
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok              At least one word written, or zero
 *                               written because FIFO was full.
 * @retval k_ra_err_invalid_arg ``channel >= 4`` or ``count == 0``.
 * @retval k_ra_err_null_ptr    ``data`` or ``out_written`` was NULL.
 *
 * @pre ``ra_ipc_init`` previously enabled the channel.
 * @pre ``data`` non-NULL and ``out_written`` non-NULL.
 * @post ``*out_written`` <= ``count``.
 * @post FIFO depth never exceeds ``k_ra_ipc_fifo_depth``.
 *
 * @note Thread safety: not thread-safe per channel.
 * @see ra_ipc_send_message
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_ipc_send_burst(uint8_t channel, const uint32_t* data, uint32_t count, uint32_t* out_written);

/**
 * @brief Pop one 32-bit word from the channel RX FIFO.
 *
 * @details
 * Reads the channel RXD register. If RDY was 0 at read time the read
 * returns zero and STA.RERR is set (HUM Ch 3.2.13 p 216). The driver
 * returns ``k_ra_err_no_data`` in that case so the caller can retry
 * once a peer has written.
 *
 * @param[in]  channel Channel id 0..3.
 * @param[out] out_msg Receives the next FIFO word; left untouched on
 *                     error.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok              Word read.
 * @retval k_ra_err_invalid_arg ``channel >= 4``.
 * @retval k_ra_err_null_ptr    ``out_msg`` was NULL.
 * @retval k_ra_err_no_data     FIFO empty at read time.
 *
 * @pre ``ra_ipc_init`` previously enabled the channel.
 * @pre Caller holds whatever lock co-ordinates this channel.
 * @post On success, FIFO advances by one word.
 * @post On empty, STA.RERR is set on the local core.
 *
 * @note Thread safety: not thread-safe per channel.
 * @see ra_ipc_send_message
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ipc_recv_message(uint8_t channel, uint32_t* out_msg);

/**
 * @brief Bounded-retry variant of ``ra_ipc_recv_message``.
 *
 * @details
 * Polls STA.RDY up to ``max_retries`` times, clearing RERR at each
 * iteration. Returns ``k_ra_err_hw_timeout`` if no word arrived in
 * the budget.
 *
 * @param[in]  channel     Channel id 0..3.
 * @param[out] out_msg     Receives the popped word on success.
 * @param[in]  max_retries Iteration cap (<= ``k_ra_ipc_retry_max``).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok               Word read.
 * @retval k_ra_err_invalid_arg  Bad channel or NULL output pointer.
 * @retval k_ra_err_hw_timeout   No word arrived in the retry budget.
 *
 * @pre ``ra_ipc_init`` previously enabled the channel.
 * @pre ``out_msg`` non-NULL.
 * @post On success, ``*out_msg`` reflects RXD.
 * @post On timeout, STA.RERR has been cleared at least once.
 *
 * @note Thread safety: not thread-safe per channel.
 * @see ra_ipc_recv_message
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_ipc_recv_message_retry(uint8_t channel, uint32_t* out_msg, uint16_t max_retries);

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
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok               Drain succeeded (may have read 0).
 * @retval k_ra_err_invalid_arg  ``channel >= 4`` or ``capacity == 0``.
 * @retval k_ra_err_null_ptr     ``out_data`` or ``out_read`` was NULL.
 *
 * @pre ``ra_ipc_init`` previously enabled the channel.
 * @pre ``out_data`` non-NULL and ``out_read`` non-NULL.
 * @post ``*out_read`` <= ``capacity``.
 * @post After the call STA.RDY may still be 1 if the peer kept writing.
 *
 * @note Thread safety: not thread-safe per channel.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_ipc_recv_burst(uint8_t channel, uint32_t* out_data, uint32_t capacity, uint32_t* out_read);

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
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok              Status read.
 * @retval k_ra_err_invalid_arg ``channel >= 4``.
 * @retval k_ra_err_null_ptr    ``out_sta`` was NULL.
 *
 * @pre Channel was initialised.
 * @pre ``out_sta`` non-NULL.
 * @post ``*out_sta`` reflects the live STA value.
 * @post No registers are mutated.
 *
 * @note Thread safety: re-entrant; pure read.
 * @see ra_ipc_clear_status
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ipc_get_status(uint8_t channel, uint32_t* out_sta);

/**
 * @brief Clear selected status bits on a channel.
 *
 * @param[in] channel Channel id 0..3.
 * @param[in] mask    Bitmask of ``ra_ipc_event_t`` flags to clear.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok              Mask written.
 * @retval k_ra_err_invalid_arg ``channel >= 4``.
 *
 * @pre Channel was initialised.
 * @pre ``mask`` only contains bits drawn from ``ra_ipc_event_t``.
 * @post Bits requested in ``mask`` read back as 0 on next get_status.
 * @post No other STA bits are disturbed.
 *
 * @note Thread safety: re-entrant per channel.
 * @see ra_ipc_get_status
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ipc_clear_status(uint8_t channel, uint32_t mask);

/**
 * @brief Clear RERR + FERR on the channel.
 *
 * @param[in] channel Channel id 0..3.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok              Errors cleared.
 * @retval k_ra_err_invalid_arg ``channel >= 4``.
 *
 * @pre Channel was initialised.
 * @pre Caller observed (or expects) STA.RERR or STA.FERR to be set.
 * @post STA.RERR == 0 and STA.FERR == 0 on next read.
 * @post No other STA bits are disturbed.
 *
 * @note Thread safety: re-entrant per channel.
 * @see ra_ipc_clear_status
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ipc_clear_errors(uint8_t channel);

/**
 * @brief Predicate: would ``ra_ipc_send_message`` succeed right now?
 *
 * @param[in]  channel    Channel id 0..3.
 * @param[out] out_can_send Receives true if FIFO has at least one free slot.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok              Predicate evaluated.
 * @retval k_ra_err_invalid_arg ``channel >= 4``.
 * @retval k_ra_err_null_ptr    ``out_can_send`` was NULL.
 *
 * @pre Channel was initialised.
 * @pre ``out_can_send`` non-NULL.
 * @post No registers are mutated.
 * @post ``*out_can_send`` reflects ``!STA.FULL``.
 *
 * @note Thread safety: re-entrant; pure read.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ipc_can_send(uint8_t channel, bool* out_can_send);

/**
 * @brief Predicate: would ``ra_ipc_recv_message`` succeed right now?
 *
 * @param[in]  channel      Channel id 0..3.
 * @param[out] out_has_data Receives true if at least one word is queued.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok              Predicate evaluated.
 * @retval k_ra_err_invalid_arg ``channel >= 4``.
 * @retval k_ra_err_null_ptr    ``out_has_data`` was NULL.
 *
 * @pre Channel was initialised.
 * @pre ``out_has_data`` non-NULL.
 * @post No registers are mutated.
 * @post ``*out_has_data`` reflects ``STA.RDY``.
 *
 * @note Thread safety: re-entrant; pure read.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ipc_has_data(uint8_t channel, bool* out_has_data);

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
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok              Attribution read.
 * @retval k_ra_err_invalid_arg ``channel >= 4``.
 * @retval k_ra_err_null_ptr    ``out_attr`` was NULL.
 *
 * @pre IPCSAR / IPCPAR are mapped (CPSCU window).
 * @pre ``out_attr`` non-NULL.
 * @post ``out_attr->secure`` reflects SAIPCIRn (1 = non-secure).
 * @post ``out_attr->privileged`` reflects PAIPCIRn (1 = unprivileged).
 *
 * @note Thread safety: re-entrant; pure read.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ipc_get_attribution(uint8_t channel, ra_ipc_attr_t* out_attr);

/**
 * @brief Read the attribution for one NMI unit.
 *
 * @details
 * IPCSAR.SAIPCNMI[0..1] and IPCPAR.PAIPCNMI[0..1] live at bits 8/9 of
 * their respective registers (HUM Ch 3.2.1 p 205-207). Provided so the
 * NMI-injection code can pre-flight before issuing IPCnNMISET writes.
 *
 * @param[in]  unit     NMI unit id 0..1 (k_ra_ipc_unit_ipc0 / ipc1).
 * @param[out] out_attr Receives ``secure`` / ``privileged`` flags.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok              Attribution read.
 * @retval k_ra_err_invalid_arg ``unit >= 2``.
 * @retval k_ra_err_null_ptr    ``out_attr`` was NULL.
 *
 * @pre ``out_attr`` non-NULL.
 * @pre IPCSAR / IPCPAR are mapped.
 * @post Output reflects SAIPCNMIu / PAIPCNMIu.
 * @post No registers are mutated.
 *
 * @note Thread safety: re-entrant; pure read.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ipc_get_nmi_attribution(uint8_t unit, ra_ipc_attr_t* out_attr);

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
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok              Attribution read.
 * @retval k_ra_err_invalid_arg ``group`` not in {0, 1}.
 * @retval k_ra_err_null_ptr    ``out_attr`` was NULL.
 *
 * @pre ``out_attr`` non-NULL.
 * @pre IPCSAR / IPCPAR are mapped.
 * @post Output reflects SAIPCSEMg / PAIPCSEMg.
 * @post No registers are mutated.
 *
 * @note Thread safety: re-entrant; pure read.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ipc_get_sem_attribution(ra_ipc_sem_attr_group_t group,
                                                  ra_ipc_attr_t*          out_attr);

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
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok              Predicate evaluated.
 * @retval k_ra_err_invalid_arg ``channel >= 4``.
 * @retval k_ra_err_null_ptr    ``out_can_access`` was NULL.
 *
 * @pre ``out_can_access`` non-NULL.
 * @pre Channel attribution registers are accessible.
 * @post ``*out_can_access`` reflects (live attr == ``required``).
 * @post No registers are mutated.
 *
 * @note Thread safety: re-entrant; pure read.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_ipc_can_access(uint8_t channel, ra_ipc_attr_t const* required, bool* out_can_access);

/* =============================================================================
 * Hardware semaphores (IPCSEM0..15)
 * =============================================================================
 */

/**
 * @brief Test-and-set acquire on one IPC hardware semaphore.
 *
 * @details
 * IPCSEMn implements test-and-set semantics: a 32-bit read sets the
 * LOCK bit and returns the previous value. A return of 0 means the
 * caller acquired the lock (it was previously unlocked). A return of
 * 1 means another core owned it.
 *
 * @param[in] sem_id Semaphore index 0..15.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok               Lock acquired.
 * @retval k_ra_err_busy         Lock already held by another core.
 * @retval k_ra_err_invalid_arg  ``sem_id >= 16``.
 *
 * @pre IPC base window is mapped.
 * @pre Caller is prepared to handle ``k_ra_err_busy`` by retry or back-off.
 * @post On success, IPCSEMn.LOCK = 1.
 * @post On busy, no register write or release is implied.
 *
 * @note Thread safety: re-entrant across cores -- this is the whole point.
 * @see ra_ipc_sem_release
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ipc_sem_try_take(uint8_t sem_id);

/**
 * @brief Bounded-spin take on one IPC hardware semaphore.
 *
 * @param[in] sem_id      Semaphore index 0..15.
 * @param[in] max_spins   Iteration cap (<= ``k_ra_ipc_sem_take_max``).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                Acquired before the budget ran out.
 * @retval k_ra_err_invalid_arg   ``sem_id >= 16``.
 * @retval k_ra_err_hw_timeout    Budget expired without acquiring.
 *
 * @pre IPC base window is mapped.
 * @pre ``max_spins`` non-zero.
 * @post On success, IPCSEMn.LOCK = 1.
 * @post On timeout, semaphore state is unchanged from last spin.
 *
 * @note Thread safety: re-entrant across cores.
 * @see ra_ipc_sem_try_take
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ipc_sem_take_timeout(uint8_t sem_id, uint16_t max_spins);

/**
 * @brief Release one IPC hardware semaphore.
 *
 * @param[in] sem_id Semaphore index 0..15.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok              Released.
 * @retval k_ra_err_invalid_arg ``sem_id >= 16``.
 *
 * @pre Caller previously acquired ``sem_id`` via ``ra_ipc_sem_try_take``
 *      or ``ra_ipc_sem_take_timeout``.
 * @pre IPC base window is mapped.
 * @post IPCSEMn.LOCK = 0.
 * @post Other waiting cores may now acquire.
 *
 * @note Thread safety: re-entrant across cores.
 * @see ra_ipc_sem_try_take
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ipc_sem_release(uint8_t sem_id);

/**
 * @brief Predicate: is the semaphore currently locked?
 *
 * @details
 * Note that simply *reading* IPCSEMn through a 32-bit access *takes*
 * the semaphore (HUM Ch 3.2.3 p 210 NOTE). This API therefore samples
 * the LOCK bit by reading and then unconditionally writes back the
 * previous value to restore the prior state -- if the semaphore was
 * unlocked the side-effect of the read is undone by the write.
 *
 * Only safe to call when the caller already owns the lock or knows
 * the peer is quiescent. Most users should treat this as a debug-only
 * helper and stick to ``ra_ipc_sem_try_take`` for real acquisition.
 *
 * @param[in]  sem_id      Semaphore index 0..15.
 * @param[out] out_locked  Receives true if the semaphore was already locked.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok               Predicate evaluated.
 * @retval k_ra_err_invalid_arg  ``sem_id >= 16``.
 * @retval k_ra_err_null_ptr     ``out_locked`` was NULL.
 *
 * @pre ``out_locked`` non-NULL.
 * @pre Caller is in a single-owner or quiescent context.
 * @post Semaphore lock state is restored to its pre-call value.
 * @post No interrupt is generated by this call.
 *
 * @note Thread safety: NOT safe under contention -- diagnostic only.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ipc_sem_is_locked(uint8_t sem_id, bool* out_locked);

/* =============================================================================
 * NMI surface (IPC0NMI* / IPC1NMI*)
 * =============================================================================
 */

/**
 * @brief Issue an inter-processor NMI to the peer core.
 *
 * @details
 * Writes 1 to IPCnNMISET.SET (HUM Ch 3.2.5 p 211 / Ch 3.2.8 p 213).
 * The peer core observes IPCnNMISTA.NMI = 1 and (if the ICU has the
 * IPCNMIn line enabled) takes the NMI exception.
 *
 * @param[in] unit NMI unit id (k_ra_ipc_unit_ipc0 or k_ra_ipc_unit_ipc1).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok              SET written.
 * @retval k_ra_err_invalid_arg ``unit >= 2``.
 *
 * @pre IPC NMI registers are mapped.
 * @pre Receiving core has unmasked the IPCNMIn vector (or expects to
 *      observe NMISTA via polling).
 * @post Peer NMISTA.NMI reads 1 until the peer writes NMICLR.CLR.
 * @post Local NMISET write occurs exactly once.
 *
 * @note Thread safety: re-entrant per unit.
 * @see ra_ipc_nmi_clear
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ipc_nmi_send(uint8_t unit);

/**
 * @brief Acknowledge an inter-processor NMI on the local core.
 *
 * @param[in] unit NMI unit id 0..1.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok              CLR written.
 * @retval k_ra_err_invalid_arg ``unit >= 2``.
 *
 * @pre IPC NMI registers are mapped.
 * @pre This core observed NMISTA.NMI = 1.
 * @post NMISTA.NMI reads 0 on next fetch.
 * @post NMI line into ICU is de-asserted.
 *
 * @note Thread safety: re-entrant per unit.
 * @see ra_ipc_nmi_send
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ipc_nmi_clear(uint8_t unit);

/**
 * @brief Read NMISTA.NMI for one IPC NMI unit.
 *
 * @param[in]  unit      NMI unit id 0..1.
 * @param[out] out_pending Receives true if NMI is currently asserted.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok              Status read.
 * @retval k_ra_err_invalid_arg ``unit >= 2``.
 * @retval k_ra_err_null_ptr    ``out_pending`` was NULL.
 *
 * @pre ``out_pending`` non-NULL.
 * @pre IPC NMI registers are mapped.
 * @post ``*out_pending`` reflects NMISTA.NMI.
 * @post No registers are mutated.
 *
 * @note Thread safety: re-entrant; pure read.
 * @see ra_ipc_nmi_send
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ipc_nmi_get_status(uint8_t unit, bool* out_pending);

/**
 * @brief Attach a callback for inter-processor NMI dispatch.
 *
 * @details
 * Single global callback shared by both NMI units; ``unit`` is passed
 * back as a parameter so the callback can demux IPC0NMI vs IPC1NMI.
 * Pass NULL to detach.
 *
 * @param[in] fn  Callback fired by ``ra_ipc_dispatch_nmi``.
 * @param[in] ctx Opaque context forwarded to ``fn``.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Always.
 *
 * @pre Caller is in single-threaded init context (single slot).
 * @pre IRQs masked at install time if NMI handler is in use.
 * @post Subsequent ``ra_ipc_dispatch_nmi`` calls invoke ``fn``.
 * @post Previous NMI callback, if any, is no longer called.
 *
 * @note Thread safety: not thread-safe (single slot).
 * @see ra_ipc_dispatch_nmi
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ipc_attach_nmi_handler(ra_ipc_nmi_fn_t fn, void* ctx);

/**
 * @brief Drive the NMI dispatch path for one unit.
 *
 * @details
 * Reads NMISTA.NMI; if set, calls the registered NMI callback and
 * writes NMICLR.CLR to acknowledge. Safe to call when no NMI is
 * pending (becomes a no-op).
 *
 * @param[in] unit NMI unit id 0..1.
 *
 * @pre Called from NMI context (or test harness).
 * @pre Either a callback is attached, or the dispatch is being used
 *      purely to ack a stuck status bit.
 * @post NMISTA.NMI reads 0 on next fetch.
 * @post Callback was invoked exactly once if it was attached and the
 *       NMI was pending.
 *
 * @note Thread safety: NMI context.
 * @see ra_ipc_attach_nmi_handler
 * @since 0.1.0
 */
void ra_ipc_dispatch_nmi(uint8_t unit);

/* =============================================================================
 * Interrupt path (maskable)
 * =============================================================================
 */

/**
 * @brief Attach a single event callback shared across all channels.
 *
 * @param[in] fn  Callback fired during dispatch. NULL to detach.
 * @param[in] ctx Opaque context forwarded to ``fn``.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Always succeeds.
 *
 * @pre Caller has not registered another callback expecting different
 *      semantics (single-slot table).
 * @pre IRQs are masked or this is single-threaded init.
 * @post ``ra_ipc_dispatch`` fires ``fn`` with ``ctx``.
 * @post The previous callback, if any, is no longer called.
 *
 * @note Thread safety: not thread-safe (single-slot global).
 * @see ra_ipc_dispatch
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ipc_attach_handler(ra_ipc_event_fn_t fn, void* ctx);

/**
 * @brief Attach a callback for a single IRQ event line on a channel.
 *
 * @details
 * Each FIFO channel exposes 8 maskable IRQ event lines (HUM Ch 3.2.10
 * p 214). This API lets each line have its own decoded callback so
 * application code does not have to walk the bitmask returned by
 * ``ra_ipc_dispatch``.
 *
 * @param[in] channel Channel id 0..3.
 * @param[in] event_id IRQ event line 0..7.
 * @param[in] fn      Callback fired when this line is observed in STA.
 *                    NULL detaches.
 * @param[in] ctx     Opaque context handed to ``fn``.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok              Stored.
 * @retval k_ra_err_invalid_arg ``channel >= 4`` or ``event_id > 7``.
 *
 * @pre Channel was initialised.
 * @pre IRQs masked at install time.
 * @post Subsequent ``ra_ipc_dispatch`` invocations call ``fn(ctx, ch, ev)``
 *       when STA.IRQev is set in the dispatched mask.
 * @post Previous handler for the line is no longer called.
 *
 * @note Thread safety: not thread-safe (per-line single slot).
 * @see ra_ipc_dispatch
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ipc_attach_event_handler(uint8_t               channel,
                                                   ra_ipc_irq_event_id_t event_id,
                                                   ra_ipc_irq_fn_t       fn,
                                                   void*                 ctx);

/**
 * @brief Drive the event callback for one channel.
 *
 * @details
 * Reads STA, masks against the per-channel event mask installed at
 * init, and calls the registered callback if any bits remain. If
 * the dispatched event includes ``k_ra_ipc_event_msg_ready`` the
 * driver pops one word from RXD and passes it to the callback.
 * Decoded per-line callbacks attached via
 * ``ra_ipc_attach_event_handler`` are invoked one per IRQn bit
 * present in the dispatched mask. Finally clears every dispatched
 * event bit via CLR.
 *
 * @param[in] channel Channel id 0..3.
 *
 * @pre ``ra_ipc_init`` was called for this channel.
 * @pre A callback was attached via ``ra_ipc_attach_handler`` or per-line
 *      ``ra_ipc_attach_event_handler`` (else the call is a no-op apart
 *      from clearing the bits).
 * @post All dispatched event bits in STA read back as 0.
 * @post FIFO advances by one word if RDY was set.
 *
 * @note Thread safety: not thread-safe per channel.
 * @see ra_ipc_attach_handler
 * @since 0.1.0
 */
void ra_ipc_dispatch(uint8_t channel);

/**
 * @brief Wire the IPC IRQ event into the ISR table.
 *
 * @details
 * Registers ``ra_ipc_dispatch_unit`` against the ELC event for the
 * matching IPC unit (``k_ra_ipc_elc_event_irq0`` for unit 0,
 * ``k_ra_ipc_elc_event_irq1`` for unit 1). The vector trampoline
 * then calls ``ra_ipc_dispatch`` for both channels in the unit
 * (channels 0+1 for unit 0; channels 2+3 for unit 1) so the per-line
 * callbacks fire on every interrupt.
 *
 * @param[in] unit     IPC unit id 0..1.
 * @param[in] priority NVIC priority forwarded to ``ra_isr_register``.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok              Registered.
 * @retval k_ra_err_invalid_arg ``unit >= 2``.
 * @retval k_ra_err_no_mem      ``ra_isr_register`` had no free slot.
 * @retval k_ra_err_exists      Already installed for that unit.
 *
 * @pre ``ra_isr_init`` has been called.
 * @pre Caller is in single-threaded init context.
 * @post NVIC line for the IPC IRQ is enabled at ``priority``.
 * @post IELSR slot for the event is owned by this driver.
 *
 * @note Thread safety: not thread-safe.
 * @see ra_ipc_uninstall_isr
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ipc_install_isr(uint8_t unit, uint8_t priority);

/**
 * @brief Tear down a previously installed IPC ISR.
 *
 * @param[in] unit IPC unit id 0..1.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok              Unregistered.
 * @retval k_ra_err_invalid_arg ``unit >= 2``.
 * @retval k_ra_err_not_found   Was not installed.
 *
 * @pre Caller is in single-threaded shutdown context.
 * @pre IRQs masked.
 * @post NVIC line for the IPC IRQ is disabled.
 * @post Driver no longer owns an IELSR slot for the unit.
 *
 * @note Thread safety: not thread-safe.
 * @see ra_ipc_install_isr
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ipc_uninstall_isr(uint8_t unit);

/* =============================================================================
 * Shared-memory ring-buffer protocol primitives
 * =============================================================================
 */

/**
 * @brief Initialise a producer/consumer ring backed by shared SRAM.
 *
 * @details
 * Zeroes the head/tail counters and stores the descriptor for later
 * ``ra_ipc_ring_produce`` / ``ra_ipc_ring_consume`` calls. Does not
 * touch the underlying ``slots`` array; the caller is responsible for
 * placing it in shared SRAM that both cores can see.
 *
 * @param[in] ring Non-NULL descriptor (caller-allocated).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok              Initialised.
 * @retval k_ra_err_null_ptr    ``ring`` or one of its required pointer
 *                               fields was NULL.
 * @retval k_ra_err_invalid_arg ``capacity == 0`` or non-power-of-two.
 *
 * @pre ``ring->slots / head / tail`` non-NULL.
 * @pre Both cores agree on the descriptor layout.
 * @post ``*ring->head == 0`` and ``*ring->tail == 0``.
 * @post Ring is empty and ready for ``ra_ipc_ring_produce``.
 *
 * @note Thread safety: caller must serialise init across cores.
 * @see ra_ipc_ring_produce
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ipc_ring_init(ra_ipc_ring_t* ring);

/**
 * @brief Push one word into the shared-memory ring (producer side).
 *
 * @details
 * Acquires ``ring->sem_id`` via ``ra_ipc_sem_try_take``, writes
 * ``payload`` into ``slots[head % capacity]``, advances head, releases
 * the semaphore, then signals the consumer through
 * ``ra_ipc_send_event(ring->channel, ring->notify_id)``.
 *
 * @param[in] ring    Non-NULL initialised descriptor.
 * @param[in] payload 32-bit word to enqueue.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok               Word enqueued and notification sent.
 * @retval k_ra_err_null_ptr     ``ring`` was NULL.
 * @retval k_ra_err_busy         Ring full or semaphore not available.
 *
 * @pre ``ra_ipc_ring_init`` succeeded.
 * @pre Producer holds no other contended IPCSEM.
 * @post On success, head advanced by 1 (mod 2^32).
 * @post On success, IRQ event line was raised on the consumer side.
 *
 * @note Thread safety: serialised by the IPCSEM wrapper.
 * @see ra_ipc_ring_consume
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ipc_ring_produce(ra_ipc_ring_t* ring, uint32_t payload);

/**
 * @brief Pop one word from the shared-memory ring (consumer side).
 *
 * @details
 * Acquires the semaphore, samples head/tail, copies
 * ``slots[tail % capacity]`` into ``out_payload`` if non-empty,
 * advances tail, releases the semaphore.
 *
 * @param[in]  ring        Non-NULL initialised descriptor.
 * @param[out] out_payload Receives the popped word.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok               Word dequeued.
 * @retval k_ra_err_null_ptr     ``ring`` or ``out_payload`` was NULL.
 * @retval k_ra_err_busy         Semaphore not available.
 * @retval k_ra_err_no_data      Ring was empty.
 *
 * @pre ``ra_ipc_ring_init`` succeeded.
 * @pre ``out_payload`` non-NULL.
 * @post On success, tail advanced by 1 (mod 2^32).
 * @post On no_data, ``*out_payload`` is left untouched.
 *
 * @note Thread safety: serialised by the IPCSEM wrapper.
 * @see ra_ipc_ring_produce
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ipc_ring_consume(ra_ipc_ring_t* ring, uint32_t* out_payload);

/**
 * @brief Predicate: ring empty?
 *
 * @param[in]  ring     Non-NULL descriptor.
 * @param[out] out_empty true when head == tail.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok              Predicate evaluated.
 * @retval k_ra_err_null_ptr    ``ring`` or ``out_empty`` was NULL.
 *
 * @pre ``ring`` and ``out_empty`` non-NULL.
 * @pre ``ring`` was initialised.
 * @post No registers are mutated.
 * @post ``*out_empty`` reflects head == tail.
 *
 * @note Thread safety: re-entrant; pure read.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ipc_ring_is_empty(const ra_ipc_ring_t* ring, bool* out_empty);

/**
 * @brief Predicate: ring full?
 *
 * @param[in]  ring     Non-NULL descriptor.
 * @param[out] out_full true when (head - tail) == capacity.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok              Predicate evaluated.
 * @retval k_ra_err_null_ptr    ``ring`` or ``out_full`` was NULL.
 *
 * @pre ``ring`` and ``out_full`` non-NULL.
 * @pre ``ring`` was initialised.
 * @post No registers are mutated.
 * @post ``*out_full`` reflects (head - tail) == capacity.
 *
 * @note Thread safety: re-entrant; pure read.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ipc_ring_is_full(const ra_ipc_ring_t* ring, bool* out_full);

#ifdef __cplusplus
}
#endif
