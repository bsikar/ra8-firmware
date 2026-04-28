/**
 * @file ra_isr.h
 * @brief NVIC + ICU IELSR allocator
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * substrate module that owns every write to the Cortex-M
 * NVIC and every write to the RA8D2 ICU's IELSR registers. Drivers
 * that need an interrupt register an ``(event, handler)`` pair
 * with ``ra_isr_register()`` and the substrate:
 *
 * 1. Allocates a free IELSR slot from an internal pool.
 * 2. Stores the caller's handler + context in a dispatch table.
 * 3. Writes the event number into the IELSR slot so the ICU
 * maps the event to the matching NVIC line.
 * 4. Sets the NVIC priority and enables the NVIC line.
 *
 * At runtime, the vector-table entry for each IELSR slot
 * calls ``ra_isr_dispatch(slot)`` which looks up the stored
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
 * - 's NSC veneer surface is ``ra_isr_*``, not the 20+
 * per-driver files that need interrupts.
 *
 * ## ELC event numbers
 *
 * The ``event`` argument is the ELC event number from HUM Ch 19
 * "Event Link Controller (ELC)" (p 817). Each peripheral has
 * well-known events (SCI0_RXI, GPT0_CCMPA, etc.). The ``ra_elc``
 * facility provides the enum.
 *
 * ## Slot lifetime
 *
 * Slots persist until explicitly unregistered. A driver that
 * wants to re-route an event calls ``ra_isr_unregister`` first.
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

#include "ra8d2_elc_regs.h"
#include "ra_err.h"

/* =============================================================================
 * Constants
 * =============================================================================
 */

/**
 * @enum ra_isr_slot_count_t
 * @brief Total number of IELSR slots tracked by the allocator.
 *
 * @details
 * RA8D2 ICU exposes 112 IELSR slots (see
 * ``ra8d2_icu_regs.h::k_ra_icu_num_ielsr``). This module tracks the
 * full 112-slot table; the substrate handles allocation
 * and the legacy ``ra_icu`` facade owns direct slot access for
 * pre-Wave-1 callers.
 */
typedef enum : uint16_t {
  k_ra_isr_slot_count = 112U, /**< Total ICU IELSR slots. */
} ra_isr_slot_count_t;

/**
 * @enum ra_isr_prio_t
 * @brief Cortex-M85 NVIC priority range.
 *
 * @details
 * NVIC priorities on the RA8D2 Cortex-M85 implement the upper 4
 * bits of the 8-bit priority register, so the effective priority
 * range is 0..15 (lower = higher priority).
 */
typedef enum : uint8_t {
  k_ra_isr_prio_max     = 15U, /**< Lowest priority. */
  k_ra_isr_prio_default = 8U,  /**< Middle priority. */
} ra_isr_prio_t;

/**
 * @enum ra_isr_invalid_slot_t
 * @brief Sentinel for "no slot allocated".
 */
typedef enum : uint16_t {
  k_ra_isr_slot_none = 0xFFFFU,
} ra_isr_invalid_slot_t;

/**
 * @typedef ra_isr_handler_t
 * @brief Driver-supplied callback invoked on interrupt entry.
 *
 * @param[in] ctx User-supplied context pointer recorded at
 * registration time.
 *
 * @note Called from handler mode on the Cortex-M85. Must return
 * quickly and must not take any ra_mstp / ra_pwr locks.
 */
typedef void (*ra_isr_handler_t)(void* ctx);

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Initialise the ra_isr table.
 *
 * @details
 * Zeros every IELSR slot, clears every dispatch-table entry, and
 * disables every NVIC line ``0..k_ra_isr_slot_count``. After this
 * call every ``ra_isr_register`` starts from a clean table.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Table cleared.
 *
 * @pre Caller is in single-threaded init context.
 * @post IELSR0..IELSR95 all zero.
 * @post NVIC lines 0..95 are disabled.
 * @post Every dispatch-table entry is (NULL handler, NULL ctx).
 *
 * @note Thread safety: not thread-safe.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_isr_init(void);

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
 * @param[in] event ELC event number from ``ra_elc_event_t``.
 * @param[in] handler Callback invoked on interrupt entry.
 * Must not be NULL.
 * @param[in] ctx Caller-supplied context handed to the
 * handler on every invocation. May be NULL.
 * @param[in] priority NVIC priority 0..k_ra_isr_prio_max.
 * @param[out] out_slot Slot number on success. May be NULL if
 * the caller does not need it.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Slot allocated, IELSR written,
 * NVIC line enabled.
 * @retval k_ra_err_null_ptr ``handler`` was NULL.
 * @retval k_ra_err_invalid_arg ``priority`` out of range.
 * @retval k_ra_err_no_mem No free IELSR slot.
 * @retval k_ra_err_exists ``event`` is already mapped by a
 * previous registration.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre ``ra_isr_init()`` has been called.
 *
 * @post On success, a single NVIC line is enabled with the given
 * priority and its IELSR entry matches ``event``.
 * @post Subsequent firings of ``event`` invoke ``handler(ctx)``
 * via the vector-table trampoline.
 *
 * @note Thread safety: not thread-safe.
 * @see ra_isr_unregister
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_isr_register(ra_elc_event_t   event,
                                       ra_isr_handler_t handler,
                                       void*            ctx,
                                       uint8_t          priority,
                                       uint16_t*        out_slot);

/**
 * @brief Release a previously-allocated IELSR slot.
 *
 * @param[in] event ELC event number to tear down.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Slot released.
 * @retval k_ra_err_not_found ``event`` was not registered.
 *
 * @pre IRQs masked or single-threaded init context.
 * @post NVIC line for the slot is disabled.
 * @post IELSR entry for the slot is zero.
 * @post Dispatch-table entry for the slot is (NULL, NULL).
 *
 * @note Thread safety: not thread-safe.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_isr_unregister(ra_elc_event_t event);

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
 * matches the IELSR slot number). ``ra_isr_dispatch`` looks up the
 * handler + context stored at registration time and calls the
 * handler with the stored context. The IELSR.IR flag is cleared
 * before the handler runs so a nested identical event re-raises
 * the interrupt instead of being latched.
 *
 * @param[in] slot Slot number 0..k_ra_isr_slot_count - 1.
 *
 * @pre Called from Cortex-M85 handler mode.
 * @pre ``slot`` has been registered via ``ra_isr_register``.
 *
 * @post IELSR.IR is cleared.
 * @post Dispatch-table handler was invoked exactly once.
 *
 * @note Thread safety: re-entrant in the sense that the handler
 * itself may enable nested interrupts; the dispatcher does
 * not take any locks.
 * @since 0.2.0
 */
void ra_isr_dispatch(uint16_t slot);

/* =============================================================================
 * Priority / enable shortcuts
 * =============================================================================
 */

/**
 * @brief Change the NVIC priority of a registered slot.
 *
 * @param[in] event ELC event number that was previously registered.
 * @param[in] priority New priority 0..k_ra_isr_prio_max.
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Priority updated.
 * @retval k_ra_err_not_found ``event`` not registered.
 * @retval k_ra_err_invalid_arg ``priority`` out of range.
 *
 * @pre IRQs masked.
 * @post NVIC IPR byte for the slot reflects the new priority.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_isr_set_priority(ra_elc_event_t event, uint8_t priority);

/**
 * @brief Look up the slot allocated to a registered event.
 *
 * @details
 * Diagnostic accessor used by unit tests to verify the allocator
 * state. Returns ``k_ra_isr_slot_none`` via ``*out_slot`` if the
 * event is not registered.
 *
 * @param[in] event ELC event number.
 * @param[out] out_slot Slot number on success, or
 * ``k_ra_isr_slot_none`` if unregistered.
 * @return ``k_ra_ok`` or ``k_ra_err_null_ptr``.
 *
 * @pre ``out_slot`` is non-NULL.
 * @post No hardware state is modified.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_isr_lookup_slot(ra_elc_event_t event, uint16_t* out_slot);

#ifdef __cplusplus
}
#endif
