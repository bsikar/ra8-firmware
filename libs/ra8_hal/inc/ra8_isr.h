/**
 * @file ra8_isr.h
 * @brief NVIC + ICU IELSR allocator
 * @ingroup grp_hal_system
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * substrate module that owns every write to the Cortex-M
 * NVIC and every write to the RA8D2 ICU's IELSR registers. Drivers
 * that need an interrupt register an ``(event, handler)`` pair
 * with ``ra8_isr_register()`` and the substrate:
 *
 * 1. Allocates a free IELSR slot from an internal pool.
 * 2. Stores the caller's handler + context in a dispatch table.
 * 3. Writes the event number into the IELSR slot so the ICU
 * maps the event to the matching NVIC line.
 * 4. Sets the NVIC priority and enables the NVIC line.
 *
 * At runtime, the vector-table entry for each IELSR slot
 * calls ``ra8_isr_dispatch(slot)`` which looks up the stored
 * handler and invokes it with its stored context. This is the
 * mechanism every + driver uses for interrupt delivery.
 *
 * ## Why centralise IELSR allocation?
 *
 * The RA ICU's "events are programmable, NVIC lines are assigned
 * at runtime" model means multiple drivers independently assigning
 * IELSR slots will collide. Putting one module in charge of the
 * free-list means:
 *
 * - Driver code says only "I want an interrupt for SCI0 RXI" --
 * it never picks an NVIC line number directly.
 * - The sequencing (write IELSR, clear pending, enable NVIC) is
 * in one place so the fencing and memory-barrier rules from
 * HUM Ch 14 are centrally enforced.
 * - 's NSC veneer surface is ``ra8_isr_*``, not the 20+
 * per-driver files that need interrupts.
 *
 * ## ELC event numbers
 *
 * The ``event`` argument is the ELC event number from HUM Ch 19
 * "Event Link Controller (ELC)" (p 817). Each peripheral has
 * well-known events (SCI0_RXI, GPT0_CCMPA, etc.). The ``ra8_elc``
 * facility provides the enum.
 *
 * ## Slot lifetime
 *
 * Slots persist until explicitly unregistered. A driver that
 * wants to re-route an event calls ``ra8_isr_unregister`` first.
 *
 * ## Threading
 *
 * Not thread-safe. Driver init runs from single-threaded init
 * context; IRQ handlers run in their own context with the caller
 * stored state but never reach the allocator.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_elc_regs.h"
#include "ra8_err.h"

/* =============================================================================
 * Constants
 * =============================================================================
 */

/**
 * @enum ra8_isr_slot_count_t
 * @brief Size of the ISR allocator's slot pool -- one slot per ICU IELSR route.
 *
 * @details
 * The RA8D2 ICU exposes ``ra8_icu_regs.h::k_ra8_icu_num_ielsr`` (96) IELSR
 * registers, each routing one ELC event to an NVIC line. The allocator keeps a
 * pool of exactly that many slots: a larger pool would let a slot be allocated
 * and its NVIC line enabled while ``ra8_icu_ielsr()`` returns NULL for it, so the
 * interrupt would be NVIC-enabled with no ICU event route (#237). A
 * ``static_assert`` in ra8_isr.c pins this count to ``k_ra8_icu_num_ielsr`` so the
 * two capacity constants cannot silently diverge.
 */
typedef enum : uint16_t {
  k_ra8_isr_slot_count = 96U, /**< One slot per ICU IELSR route (= k_ra8_icu_num_ielsr). */
} ra8_isr_slot_count_t;

/**
 * @enum ra8_isr_prio_t
 * @brief Cortex-M85 NVIC priority range.
 *
 * @details
 * NVIC priorities on the RA8D2 Cortex-M85 implement the upper 4
 * bits of the 8-bit priority register, so the effective priority
 * range is 0..15 (lower = higher priority).
 */
typedef enum : uint8_t {
  k_ra8_isr_prio_max     = 15U, /**< Lowest priority. */
  k_ra8_isr_prio_default = 8U,  /**< Middle priority. */
} ra8_isr_prio_t;

/**
 * @enum ra8_isr_invalid_slot_t
 * @brief Sentinel for "no slot allocated".
 */
typedef enum : uint16_t {
  k_ra8_isr_slot_none = 0xFFFFU, /**< RA8 ISR slot none. */
} ra8_isr_invalid_slot_t;

/**
 * @typedef ra8_isr_handler_t
 * @brief Driver-supplied callback invoked on interrupt entry.
 *
 * @param[in] ctx User-supplied context pointer recorded at
 * registration time.
 *
 * @note Called from handler mode on the Cortex-M85. Must return
 * quickly and must not take any ra8_mstp / ra8_pwr locks.
 */
typedef void (*ra8_isr_handler_t)(void* ctx);

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Initialise the ra8_isr table.
 *
 * @details
 * Zeros every IELSR slot, clears every dispatch-table entry, and
 * disables every NVIC line ``0..k_ra8_isr_slot_count``. After this
 * call every ``ra8_isr_register`` starts from a clean table.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Table cleared.
 *
 * @pre Caller is in single-threaded init context.
 * @post IELSR0..IELSR95 all zero.
 * @post NVIC lines 0..95 are disabled.
 * @post Every dispatch-table entry is (NULL handler, NULL ctx).
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_isr_init(void);

/* =============================================================================
 * Registration
 * =============================================================================
 */

/**
 * @brief Allocate an IELSR slot for an ELC event + handler.
 *
 * @details
 * Walks the free list, picks the first open slot, writes the event
 * number into the corresponding IELSR register, stores
 * ``(handler, ctx)`` in the dispatch table, clears the pending
 * bit in the NVIC, sets the priority, and enables the NVIC line.
 *
 * On success, ``*out_slot`` holds the assigned slot number. The
 * caller normally ignores this value and only cares that the
 * interrupt will fire; the slot number is useful for unregistration
 * and for diagnostic dumps.
 *
 * @param[in] event ELC event number from ``ra8_elc_event_t``.
 * @param[in] handler Callback invoked on interrupt entry.
 * Must not be NULL.
 * @param[in] ctx Caller-supplied context handed to the
 * handler on every invocation. May be NULL.
 * @param[in] priority NVIC priority 0..k_ra8_isr_prio_max.
 * @param[out] out_slot Slot number on success. May be NULL if
 * the caller does not need it.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Slot allocated, IELSR written,
 * NVIC line enabled.
 * @retval k_ra8_err_null_ptr ``handler`` was NULL.
 * @retval k_ra8_err_invalid_arg ``priority`` out of range.
 * @retval k_ra8_err_no_mem No free IELSR slot.
 * @retval k_ra8_err_exists ``event`` is already mapped by a
 * previous registration.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre ``ra8_isr_init()`` has been called.
 *
 * @post On success, a single NVIC line is enabled with the given
 * priority and its IELSR entry matches ``event``.
 * @post Subsequent firings of ``event`` invoke ``handler(ctx)``
 * via the vector-table trampoline.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_isr_unregister
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_isr_register(ra8_elc_event_t   event,
                                         ra8_isr_handler_t handler,
                                         void*             ctx,
                                         uint8_t           priority,
                                         uint16_t*         out_slot);

/**
 * @brief Release a previously-allocated IELSR slot.
 *
 * @param[in] event ELC event number to tear down.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Slot released.
 * @retval k_ra8_err_not_found ``event`` was not registered.
 *
 * @pre IRQs masked or single-threaded init context.
 * @post NVIC line for the slot is disabled.
 * @post IELSR entry for the slot is zero.
 * @post Dispatch-table entry for the slot is (NULL, NULL).
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_isr_unregister(ra8_elc_event_t event);

/* =============================================================================
 * Dispatch
 * =============================================================================
 */

/**
 * @brief Invoke the handler stored in dispatch-table slot ``slot``.
 *
 * @details
 * Called from the Cortex-M85 vector-table trampoline when any ICU
 * NVIC line fires. The trampoline feeds its NVIC index (which
 * matches the IELSR slot number). ``ra8_isr_dispatch`` looks up the
 * handler + context stored at registration time and calls the
 * handler with the stored context. The IELSR.IR flag is cleared
 * before the handler runs so a nested identical event re-raises
 * the interrupt instead of being latched.
 *
 * @param[in] slot Slot number 0..k_ra8_isr_slot_count - 1.
 *
 * @pre Called from Cortex-M85 handler mode.
 * @pre ``slot`` has been registered via ``ra8_isr_register``.
 *
 * @post IELSR.IR is cleared.
 * @post Dispatch-table handler was invoked exactly once.
 *
 * @note Thread safety: re-entrant in the sense that the handler
 * itself may enable nested interrupts; the dispatcher does
 * not take any locks.
 * @since 0.1.0
 */
void ra8_isr_dispatch(uint16_t slot);

/* =============================================================================
 * Priority / enable shortcuts
 * =============================================================================
 */

/**
 * @brief Change the NVIC priority of a registered slot.
 *
 * @param[in] event ELC event number that was previously registered.
 * @param[in] priority New priority 0..k_ra8_isr_prio_max.
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Priority updated.
 * @retval k_ra8_err_not_found ``event`` not registered.
 * @retval k_ra8_err_invalid_arg ``priority`` out of range.
 *
 * @pre IRQs masked.
 * @post NVIC IPR byte for the slot reflects the new priority.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_isr_set_priority(ra8_elc_event_t event, uint8_t priority);

/**
 * @brief Look up the slot allocated to a registered event.
 *
 * @details
 * Diagnostic accessor used by unit tests to verify the allocator
 * state. Returns ``k_ra8_isr_slot_none`` via ``*out_slot`` if the
 * event is not registered.
 *
 * @param[in] event ELC event number.
 * @param[out] out_slot Slot number on success, or
 * ``k_ra8_isr_slot_none`` if unregistered.
 * @return ``k_ra8_ok`` or ``k_ra8_err_null_ptr``.
 *
 * @pre ``out_slot`` is non-NULL.
 * @post No hardware state is modified.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_isr_lookup_slot(ra8_elc_event_t event, uint16_t* out_slot);

/**
 * @brief Enable or disable DTC activation on a registered IELSR slot.
 *
 * @details
 * The Data Transfer Controller has no software-start register (HUM Ch 18.3
 * p 796: "The DTC is activated by an interrupt request"); a peripheral or
 * ELC event activates it only when that event's ICU.IELSRn slot has its
 * ``DTCE`` bit (bit 24) set. With ``DTCE`` clear the same event is taken by
 * the CPU as an ordinary interrupt instead. Because ``ra8_isr`` owns every
 * IELSR slot (``ra8_isr_register`` writes the ``IELS`` event field, the
 * dispatcher clears ``IR``), arming or disarming DTC on an allocated slot
 * belongs here rather than open-coded in every DTC application.
 *
 * The write is a read-modify-write that touches only ``DTCE``: the ``IELS``
 * event-select field written by ::ra8_isr_register is preserved, and the
 * write-0-to-clear ``IR`` status flag is left untouched -- its own read
 * value is written back, which retains it (see ::ra8_isr_dispatch and issue
 * #170). The DTC clears ``DTCE`` itself when a block completes (HUM Figure
 * 18.5 p 801), so a repeating transfer re-arms with ``enable = true``
 * before each activation.
 *
 * @param[in] slot   IELSR slot 0..k_ra8_isr_slot_count - 1, as returned by
 *                   ::ra8_isr_register through its ``out_slot`` parameter.
 * @param[in] enable ``true`` sets ``DTCE`` (route the event to the DTC);
 *                   ``false`` clears it (route the event to the CPU).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              ``IELSRn.DTCE`` for the slot now equals
 *                               @p enable.
 * @retval k_ra8_err_invalid_arg ``slot`` is out of range.
 * @retval k_ra8_err_not_found   ``slot`` is not currently registered.
 * @retval k_ra8_err_hw_error    The IELSR accessor returned NULL.
 *
 * @pre ``slot`` was assigned by a prior ::ra8_isr_register call.
 * @pre IRQs masked or single-threaded init context.
 * @post On ``k_ra8_ok``, ``IELSRn.DTCE`` for the slot reflects @p enable.
 * @post No other IELSR field (``IELS`` / ``IR``) is modified.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_isr_register
 * @see ra8_isr_dispatch
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_isr_set_dtc(uint16_t slot, bool enable);

/**
 * @brief Globally enable maskable interrupts (PRIMASK = 0).
 *
 * @details
 * The Cortex-M85 boots with PRIMASK clear, but ``SystemInit`` masks
 * IRQs with ``cpsid i`` to give the application a quiet boot window
 * for driver setup. Once every IRQ source has been registered via
 * ``ra8_isr_register`` and the application is ready to start servicing
 * interrupts, call this to drop the global mask. Standard CMSIS
 * convention is "mask at boot, unmask once main() has finished
 * deterministic init".
 *
 * @pre Every IRQ source the application uses has been wired up.
 * @pre Every shared data structure that ISRs touch is initialized.
 * @post PRIMASK is clear; pending NVIC interrupts will dispatch.
 *
 * @note Symmetric counterpart of ``ra8_isr_globals_disable``. Calling
 *       ``ra8_isr_globals_enable`` while PRIMASK is already clear is a
 *       safe no-op.
 *
 * @since 0.1.0
 *
 * @post Caller-visible state matches the documented contract.
 */
void ra8_isr_globals_enable(void);

/**
 * @brief Globally mask maskable interrupts (PRIMASK = 1).
 *
 * @details
 * Companion to ``ra8_isr_globals_enable``. Used by the application
 * around critical sections where an ISR firing mid-update would
 * corrupt shared state (the per-driver ``ra8_register_guard.h``
 * helpers wrap this for short scopes; this function exists for
 * application-level sequencing during shutdown / reset).
 *
 * @pre None.
 * @post PRIMASK is set; subsequent NVIC interrupts will pend until
 *       PRIMASK is cleared again.
 *
 * @note Faults still fire (NMI / HardFault are not maskable via
 *       PRIMASK). Use FAULTMASK if you need that, but the project
 *       does not expose a wrapper for it.
 *
 * @since 0.1.0
 *
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 */
void ra8_isr_globals_disable(void);

#ifdef __cplusplus
}
#endif
