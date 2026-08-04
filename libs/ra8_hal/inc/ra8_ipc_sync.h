/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_ipc_sync.h
 * @brief Inter-Processor Communication (IPC) HAL driver -- sync API
 * @ingroup grp_hal_system
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Hardware-semaphore, NMI-surface, maskable-interrupt dispatch, and
 * shared-memory ring-buffer protocol prototypes for the RA8D2 IPC mailbox
 * driver. Split out of ``ra8_ipc.h`` so each header stays within the
 * repository file-size budget; ``ra8_ipc.h`` re-includes this header so
 * existing consumers are unaffected.
 *
 *
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_ipc_regs.h"
#include "ra8_ipc_types.h"

/* =============================================================================
 * Cross-core ordering barrier
 * =============================================================================
 */

/**
 * @brief Cross-core data-memory barrier for IPC publish + lock ordering.
 *
 * @details
 * Issues a full-system Data Memory Barrier (``DMB SY``) so a producer's
 * Normal-memory payload and ring-index stores -- and the accesses inside an
 * IPCSEM critical section -- become observable to the peer core BEFORE the
 * Device-mapped FIFO/IRQ notification (``TXD`` / ``ISET``) or the IPCSEM
 * release the peer wakes on. Without it the Cortex-M85 may post the
 * notification ahead of the SRAM payload store, letting the Cortex-M33
 * consumer observe the new head index while the payload is still in the
 * write buffer -- a stale or torn message. The hand-rolled dual-core mailbox
 * examples open-code the same ``dsb``; this is the reusable-HAL equivalent
 * (a ``DMB`` suffices: it orders accesses without waiting for completion the
 * way ``DSB`` would).
 *
 * @return void.
 *
 * @pre The IPC peripheral is mapped (no access is performed by this call).
 * @pre Caller runs on the producing or consuming core (any privilege level).
 * @post All memory accesses sequenced before the call are observed before
 *       any sequenced after it, by every observer in the system domain.
 * @post No architectural state other than memory ordering is affected.
 *
 * @note Safe in interrupt context: one barrier instruction, no memory
 *       access, no lock taken.
 * @note On a host build (``RA8_OFF_TARGET``) the fake cores share a
 *       single-threaded address space, so the barrier compiles to a no-op.
 * @since 0.1.0
 */
RA8_ISR_SAFE
static inline void ra8_ipc_barrier(void)
{
#ifndef RA8_OFF_TARGET
  __asm__ volatile("dmb 0xF" ::: "memory");
#endif
}

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
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               Lock acquired.
 * @retval k_ra8_err_busy         Lock already held by another core.
 * @retval k_ra8_err_invalid_arg  ``sem_id >= 16``.
 *
 * @pre IPC base window is mapped.
 * @pre Caller is prepared to handle ``k_ra8_err_busy`` by retry or back-off.
 * @post On success, IPCSEMn.LOCK = 1.
 * @post On busy, no register write or release is implied.
 *
 * @note Thread safety: re-entrant across cores -- this is the whole point.
 * @see ra8_ipc_sem_release
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ipc_sem_try_take(uint8_t sem_id);

/**
 * @brief Bounded-spin take on one IPC hardware semaphore.
 *
 * @param[in] sem_id      Semaphore index 0..15.
 * @param[in] max_spins   Iteration cap (<= ``k_ra8_ipc_sem_take_max``).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Acquired before the budget ran out.
 * @retval k_ra8_err_invalid_arg   ``sem_id >= 16``.
 * @retval k_ra8_err_hw_timeout    Budget expired without acquiring.
 *
 * @pre IPC base window is mapped.
 * @pre ``max_spins`` non-zero.
 * @post On success, IPCSEMn.LOCK = 1.
 * @post On timeout, semaphore state is unchanged from last spin.
 *
 * @note Thread safety: re-entrant across cores.
 * @see ra8_ipc_sem_try_take
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ipc_sem_take_timeout(uint8_t sem_id, uint16_t max_spins);

/**
 * @brief Release one IPC hardware semaphore.
 *
 * @param[in] sem_id Semaphore index 0..15.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Released.
 * @retval k_ra8_err_invalid_arg ``sem_id >= 16``.
 *
 * @pre Caller previously acquired ``sem_id`` via ``ra8_ipc_sem_try_take``
 *      or ``ra8_ipc_sem_take_timeout``.
 * @pre IPC base window is mapped.
 * @post IPCSEMn.LOCK = 0.
 * @post Other waiting cores may now acquire.
 *
 * @note Thread safety: re-entrant across cores.
 * @see ra8_ipc_sem_try_take
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ipc_sem_release(uint8_t sem_id);

/**
 * @brief Predicate: is the semaphore currently locked?
 *
 * @details
 * Note that simply *reading* IPCSEMn through a 32-bit access *takes*
 * the semaphore (HUM Ch 3.2.3 p 210 NOTE). This API therefore samples
 * the LOCK bit by reading and, if the read observed "unlocked", undoes
 * the probe's own take with the write-1-to-clear release command so
 * the register reads back 0 again.
 *
 * Only safe to call when the caller already owns the lock or knows
 * the peer is quiescent. Most users should treat this as a debug-only
 * helper and stick to ``ra8_ipc_sem_try_take`` for real acquisition.
 *
 * @param[in]  sem_id      Semaphore index 0..15.
 * @param[out] out_locked  Receives true if the semaphore was already locked.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               Predicate evaluated.
 * @retval k_ra8_err_invalid_arg  ``sem_id >= 16``.
 * @retval k_ra8_err_null_ptr     ``out_locked`` was NULL.
 *
 * @pre ``out_locked`` non-NULL.
 * @pre Caller is in a single-owner or quiescent context.
 * @post Semaphore lock state is restored to its pre-call value.
 * @post No interrupt is generated by this call.
 *
 * @note Thread safety: NOT safe under contention -- diagnostic only.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ipc_sem_is_locked(uint8_t sem_id, bool* out_locked);

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
 * @param[in] unit NMI unit id (k_ra8_ipc_unit_ipc0 or k_ra8_ipc_unit_ipc1).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              SET written.
 * @retval k_ra8_err_invalid_arg ``unit >= 2``.
 *
 * @pre IPC NMI registers are mapped.
 * @pre Receiving core has unmasked the IPCNMIn vector (or expects to
 *      observe NMISTA via polling).
 * @post Peer NMISTA.NMI reads 1 until the peer writes NMICLR.CLR.
 * @post Local NMISET write occurs exactly once.
 *
 * @note Thread safety: re-entrant per unit.
 * @see ra8_ipc_nmi_clear
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ipc_nmi_send(uint8_t unit);

/**
 * @brief Acknowledge an inter-processor NMI on the local core.
 *
 * @param[in] unit NMI unit id 0..1.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              CLR written.
 * @retval k_ra8_err_invalid_arg ``unit >= 2``.
 *
 * @pre IPC NMI registers are mapped.
 * @pre This core observed NMISTA.NMI = 1.
 * @post NMISTA.NMI reads 0 on next fetch.
 * @post NMI line into ICU is de-asserted.
 *
 * @note Thread safety: re-entrant per unit.
 * @see ra8_ipc_nmi_send
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ipc_nmi_clear(uint8_t unit);

/**
 * @brief Read NMISTA.NMI for one IPC NMI unit.
 *
 * @param[in]  unit      NMI unit id 0..1.
 * @param[out] out_pending Receives true if NMI is currently asserted.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Status read.
 * @retval k_ra8_err_invalid_arg ``unit >= 2``.
 * @retval k_ra8_err_null_ptr    ``out_pending`` was NULL.
 *
 * @pre ``out_pending`` non-NULL.
 * @pre IPC NMI registers are mapped.
 * @post ``*out_pending`` reflects NMISTA.NMI.
 * @post No registers are mutated.
 *
 * @note Thread safety: re-entrant; pure read.
 * @see ra8_ipc_nmi_send
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ipc_nmi_get_status(uint8_t unit, bool* out_pending);

/**
 * @brief Attach a callback for inter-processor NMI dispatch.
 *
 * @details
 * Single global callback shared by both NMI units; ``unit`` is passed
 * back as a parameter so the callback can demux IPC0NMI vs IPC1NMI.
 * Pass NULL to detach.
 *
 * @param[in] fn  Callback fired by ``ra8_ipc_dispatch_nmi``.
 * @param[in] ctx Opaque context forwarded to ``fn``.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Always.
 *
 * @pre Caller is in single-threaded init context (single slot).
 * @pre IRQs masked at install time if NMI handler is in use.
 * @post Subsequent ``ra8_ipc_dispatch_nmi`` calls invoke ``fn``.
 * @post Previous NMI callback, if any, is no longer called.
 *
 * @note Thread safety: not thread-safe (single slot).
 * @see ra8_ipc_dispatch_nmi
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ipc_attach_nmi_handler(ra8_ipc_nmi_fn_t fn, void* ctx);

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
 * @see ra8_ipc_attach_nmi_handler
 * @since 0.1.0
 */
void ra8_ipc_dispatch_nmi(uint8_t unit);

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
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Always succeeds.
 *
 * @pre Caller has not registered another callback expecting different
 *      semantics (single-slot table).
 * @pre IRQs are masked or this is single-threaded init.
 * @post ``ra8_ipc_dispatch`` fires ``fn`` with ``ctx``.
 * @post The previous callback, if any, is no longer called.
 *
 * @note Thread safety: not thread-safe (single-slot global).
 * @see ra8_ipc_dispatch
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ipc_attach_handler(ra8_ipc_event_fn_t fn, void* ctx);

/**
 * @brief Attach a callback for a single IRQ event line on a channel.
 *
 * @details
 * Each FIFO channel exposes 8 maskable IRQ event lines (HUM Ch 3.2.10
 * p 214). This API lets each line have its own decoded callback so
 * application code does not have to walk the bitmask returned by
 * ``ra8_ipc_dispatch``.
 *
 * @param[in] channel Channel id 0..3.
 * @param[in] event_id IRQ event line 0..7.
 * @param[in] fn      Callback fired when this line is observed in STA.
 *                    NULL detaches.
 * @param[in] ctx     Opaque context handed to ``fn``.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Stored.
 * @retval k_ra8_err_invalid_arg ``channel >= 4`` or ``event_id > 7``.
 *
 * @pre Channel was initialized.
 * @pre IRQs masked at install time.
 * @post Subsequent ``ra8_ipc_dispatch`` invocations call ``fn(ctx, ch, ev)``
 *       when STA.IRQev is set in the dispatched mask.
 * @post Previous handler for the line is no longer called.
 *
 * @note Thread safety: not thread-safe (per-line single slot).
 * @see ra8_ipc_dispatch
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ipc_attach_event_handler(uint8_t                channel,
                                                     ra8_ipc_irq_event_id_t event_id,
                                                     ra8_ipc_irq_fn_t       fn,
                                                     void*                  ctx);

/**
 * @brief Drive the event callback for one channel.
 *
 * @details
 * Reads STA, masks against the per-channel event mask installed at
 * init, and calls the registered callback if any bits remain. If
 * the dispatched event includes ``k_ra8_ipc_event_msg_ready`` the
 * driver pops one word from RXD and passes it to the callback.
 * Decoded per-line callbacks attached via
 * ``ra8_ipc_attach_event_handler`` are invoked one per IRQn bit
 * present in the dispatched mask. Finally clears every dispatched
 * event bit via CLR.
 *
 * @param[in] channel Channel id 0..3.
 *
 * @pre ``ra8_ipc_init`` was called for this channel.
 * @pre A callback was attached via ``ra8_ipc_attach_handler`` or per-line
 *      ``ra8_ipc_attach_event_handler`` (else the call is a no-op apart
 *      from clearing the bits).
 * @post All dispatched event bits in STA read back as 0.
 * @post FIFO advances by one word if RDY was set.
 *
 * @note Thread safety: not thread-safe per channel.
 * @see ra8_ipc_attach_handler
 * @since 0.1.0
 */
void ra8_ipc_dispatch(uint8_t channel);

/**
 * @brief Wire the IPC IRQ event into the ISR table.
 *
 * @details
 * Registers ``ra8_ipc_dispatch_unit`` against the ELC event for the
 * matching IPC unit (``k_ra8_ipc_elc_event_irq0`` for unit 0,
 * ``k_ra8_ipc_elc_event_irq1`` for unit 1). The vector trampoline
 * then calls ``ra8_ipc_dispatch`` for both channels in the unit
 * (channels 0+1 for unit 0; channels 2+3 for unit 1) so the per-line
 * callbacks fire on every interrupt.
 *
 * @param[in] unit     IPC unit id 0..1.
 * @param[in] priority NVIC priority forwarded to ``ra8_isr_register``.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Registered.
 * @retval k_ra8_err_invalid_arg ``unit >= 2``.
 * @retval k_ra8_err_no_mem      ``ra8_isr_register`` had no free slot.
 * @retval k_ra8_err_exists      Already installed for that unit.
 *
 * @pre ``ra8_isr_init`` has been called.
 * @pre Caller is in single-threaded init context.
 * @post NVIC line for the IPC IRQ is enabled at ``priority``.
 * @post IELSR slot for the event is owned by this driver.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_ipc_uninstall_isr
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ipc_install_isr(uint8_t unit, uint8_t priority);

/**
 * @brief Tear down a previously installed IPC ISR.
 *
 * @param[in] unit IPC unit id 0..1.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Unregistered.
 * @retval k_ra8_err_invalid_arg ``unit >= 2``.
 * @retval k_ra8_err_not_found   Was not installed.
 *
 * @pre Caller is in single-threaded shutdown context.
 * @pre IRQs masked.
 * @post NVIC line for the IPC IRQ is disabled.
 * @post Driver no longer owns an IELSR slot for the unit.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_ipc_install_isr
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ipc_uninstall_isr(uint8_t unit);

/* =============================================================================
 * Shared-memory ring-buffer protocol primitives
 * =============================================================================
 */

/**
 * @brief Initialise a producer/consumer ring backed by shared SRAM.
 *
 * @details
 * Zeroes the head/tail counters and stores the descriptor for later
 * ``ra8_ipc_ring_produce`` / ``ra8_ipc_ring_consume`` calls. Does not
 * touch the underlying ``slots`` array; the caller is responsible for
 * placing it in shared SRAM that both cores can see.
 *
 * @param[in] ring Non-NULL descriptor (caller-allocated).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Initialized.
 * @retval k_ra8_err_null_ptr    ``ring`` or one of its required pointer
 *                               fields was NULL.
 * @retval k_ra8_err_invalid_arg ``capacity == 0`` or non-power-of-two.
 *
 * @pre ``ring->slots / head / tail`` non-NULL.
 * @pre Both cores agree on the descriptor layout.
 * @post ``*ring->head == 0`` and ``*ring->tail == 0``.
 * @post Ring is empty and ready for ``ra8_ipc_ring_produce``.
 *
 * @note Thread safety: caller must serialise init across cores.
 * @see ra8_ipc_ring_produce
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ipc_ring_init(ra8_ipc_ring_t* ring);

/**
 * @brief Push one word into the shared-memory ring (producer side).
 *
 * @details
 * Acquires ``ring->sem_id`` via ``ra8_ipc_sem_try_take``, writes
 * ``payload`` into ``slots[head % capacity]``, advances head, releases
 * the semaphore, then signals the consumer through
 * ``ra8_ipc_send_event(ring->channel, ring->notify_id)``.
 *
 * @param[in] ring    Non-NULL initialized descriptor.
 * @param[in] payload 32-bit word to enqueue.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               Word enqueued and notification sent.
 * @retval k_ra8_err_null_ptr     ``ring`` was NULL.
 * @retval k_ra8_err_busy         Ring full or semaphore not available.
 *
 * @pre ``ra8_ipc_ring_init`` succeeded.
 * @pre Producer holds no other contended IPCSEM.
 * @post On success, head advanced by 1 (mod 2^32).
 * @post On success, IRQ event line was raised on the consumer side.
 *
 * @note Thread safety: serialised by the IPCSEM wrapper.
 * @see ra8_ipc_ring_consume
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ipc_ring_produce(ra8_ipc_ring_t* ring, uint32_t payload);

/**
 * @brief Pop one word from the shared-memory ring (consumer side).
 *
 * @details
 * Acquires the semaphore, samples head/tail, copies
 * ``slots[tail % capacity]`` into ``out_payload`` if non-empty,
 * advances tail, releases the semaphore.
 *
 * @param[in]  ring        Non-NULL initialized descriptor.
 * @param[out] out_payload Receives the popped word.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               Word dequeued.
 * @retval k_ra8_err_null_ptr     ``ring`` or ``out_payload`` was NULL.
 * @retval k_ra8_err_busy         Semaphore not available.
 * @retval k_ra8_err_no_data      Ring was empty.
 *
 * @pre ``ra8_ipc_ring_init`` succeeded.
 * @pre ``out_payload`` non-NULL.
 * @post On success, tail advanced by 1 (mod 2^32).
 * @post On no_data, ``*out_payload`` is left untouched.
 *
 * @note Thread safety: serialised by the IPCSEM wrapper.
 * @see ra8_ipc_ring_produce
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ipc_ring_consume(ra8_ipc_ring_t* ring, uint32_t* out_payload);

/**
 * @brief Predicate: ring empty?
 *
 * @param[in]  ring     Non-NULL descriptor.
 * @param[out] out_empty true when head == tail.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Predicate evaluated.
 * @retval k_ra8_err_null_ptr    ``ring`` or ``out_empty`` was NULL.
 *
 * @pre ``ring`` and ``out_empty`` non-NULL.
 * @pre ``ring`` was initialized.
 * @post No registers are mutated.
 * @post ``*out_empty`` reflects head == tail.
 *
 * @note Thread safety: re-entrant; pure read.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ipc_ring_is_empty(const ra8_ipc_ring_t* ring, bool* out_empty);

/**
 * @brief Predicate: ring full?
 *
 * @param[in]  ring     Non-NULL descriptor.
 * @param[out] out_full true when (head - tail) == capacity.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Predicate evaluated.
 * @retval k_ra8_err_null_ptr    ``ring`` or ``out_full`` was NULL.
 *
 * @pre ``ring`` and ``out_full`` non-NULL.
 * @pre ``ring`` was initialized.
 * @post No registers are mutated.
 * @post ``*out_full`` reflects (head - tail) == capacity.
 *
 * @note Thread safety: re-entrant; pure read.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ipc_ring_is_full(const ra8_ipc_ring_t* ring, bool* out_full);

#ifdef __cplusplus
}
#endif
