/**
 * @file ra8_isr.c
 * @brief NVIC + ICU IELSR allocator implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Ring 3 / HAL substrate. Owns every write to the ICU IELSR slot array
 * and to the Cortex-M85 NVIC ISER / ICER / IPR windows. See
 * ``ra8_isr.h`` for the API contract.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_isr.h"

#include <stdint.h>

#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_hw_intrinsics.h"
#include "ra8_icu_regs.h"
#include "ra8_log.h"

static const char* s_tag = "ISR";

/**
 * @enum ra8_isr_nvic_t
 * @brief NVIC register-base offsets used by the dispatcher.
 *
 * @details
 * Cortex-M85 architectural layout from the Armv8.1-M reference
 * manual. The same addresses apply on the Cortex-M33 secondary
 * core; that is why this module is a substrate module and not a
 * target-specific peripheral driver.
 */
typedef enum : uintptr_t {
  k_ra8_isr_nvic_iser_base = 0xE000E100UL, /**< ISER base: enables.       */
  k_ra8_isr_nvic_icer_base = 0xE000E180UL, /**< ICER base: disables.      */
  k_ra8_isr_nvic_icpr_base = 0xE000E280UL, /**< ICPR base: clear pending. */
  k_ra8_isr_nvic_ipr_base  = 0xE000E400UL, /**< IPR base: priority bytes. */
} ra8_isr_nvic_t;

/**
 * @enum ra8_isr_nvic_layout_t
 * @brief Bit-per-word geometry of ISER / ICER arrays.
 */
typedef enum : uint16_t {
  k_ra8_isr_nvic_bits_per_word = 32U, /**< 32 bits per ISER/ICER entry. */
  k_ra8_isr_nvic_prio_shift    = 4U,  /**< Upper 4 priority bits used.  */
} ra8_isr_nvic_layout_t;

/**
 * @struct ra8_isr_slot_t
 * @brief Dispatch-table entry for one IELSR slot.
 */
typedef struct {
  ra8_isr_handler_t handler;  /**< Driver callback, NULL if free. */
  void*             ctx;      /**< Context passed to handler.     */
  ra8_elc_event_t   event;    /**< Event mapped to this slot.     */
  uint8_t           priority; /**< Last-set NVIC priority.        */
  bool              in_use;   /**< True when allocated.           */
} ra8_isr_slot_t;

/**
 * @var s_slots
 * @brief Per-slot dispatch table.
 *
 * @details
 * Indexed by slot number 0..k_ra8_isr_slot_count - 1. Updated by
 * ``ra8_isr_register / unregister`` and read by ``ra8_isr_dispatch``.
 * Static so it is zero-initialized at boot.
 */
static ra8_isr_slot_t s_slots[k_ra8_isr_slot_count];

/* The slot pool must not exceed the ICU IELSR capacity: a slot with index
 * >= k_ra8_icu_num_ielsr could be allocated and its NVIC line enabled, but
 * ra8_icu_ielsr() returns NULL for it, leaving the interrupt NVIC-enabled with
 * no ICU event route (#237). Pin the two capacity constants together so they
 * cannot silently diverge. */
static_assert((uint16_t)k_ra8_isr_slot_count == (uint16_t)k_ra8_icu_num_ielsr,
              "ra8_isr slot pool must equal the ICU IELSR capacity (see #237)");

/* =============================================================================
 * NVIC pokes (no-op on host)
 * =============================================================================
 *
 * The fake maps the SCB / NVIC window via ra8_fake_mmap (core
 * region base 0xE0000000, size 0x100000). Writes land in host
 * RAM; the tests observe the register state via the same aliases.
 */

/**
 * @brief Enable NVIC line ``n``.
 *
 * @details See implementation.
 * @param[in] n See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void internal_nvic_enable(uint16_t n)
{
  const uint16_t     word = n / k_ra8_isr_nvic_bits_per_word;
  const uint16_t     bit  = n % k_ra8_isr_nvic_bits_per_word;
  volatile uint32_t* iser =
    (volatile uint32_t*)(k_ra8_isr_nvic_iser_base + ((uintptr_t)word * sizeof(uint32_t)));
  *iser = (uint32_t)1UL << bit;
}

/**
 * @brief Disable NVIC line ``n``.
 *
 * @details See implementation.
 * @param[in] n See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void internal_nvic_disable(uint16_t n)
{
  const uint16_t     word = n / k_ra8_isr_nvic_bits_per_word;
  const uint16_t     bit  = n % k_ra8_isr_nvic_bits_per_word;
  volatile uint32_t* icer =
    (volatile uint32_t*)(k_ra8_isr_nvic_icer_base + ((uintptr_t)word * sizeof(uint32_t)));
  *icer = (uint32_t)1UL << bit;
}

/**
 * @brief Clear pending flag on NVIC line ``n``.
 *
 * @details See implementation.
 * @param[in] n See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void internal_nvic_clear_pending(uint16_t n)
{
  const uint16_t     word = n / k_ra8_isr_nvic_bits_per_word;
  const uint16_t     bit  = n % k_ra8_isr_nvic_bits_per_word;
  volatile uint32_t* icpr =
    (volatile uint32_t*)(k_ra8_isr_nvic_icpr_base + ((uintptr_t)word * sizeof(uint32_t)));
  *icpr = (uint32_t)1UL << bit;
}

/**
 * @brief Set NVIC priority byte for line ``n``.
 *
 * @details See implementation.
 * @param[in] n See implementation.
 * @param[in] prio See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void internal_nvic_set_priority(uint16_t n, uint8_t prio)
{
  volatile uint8_t* ipr = (volatile uint8_t*)(k_ra8_isr_nvic_ipr_base + (uintptr_t)n);
  *ipr                  = (uint8_t)(prio << k_ra8_isr_nvic_prio_shift);
}

/**
 * @brief Write the event number into an IELSR slot.
 *
 * @details
 * The 9-bit event number goes into bits 8..0 per HUM Ch 14.2.x
 * (IELSRn layout). Upper bits (DTC enable, IR status) are written
 * zero so the slot starts clean.
 *
 * @param[in] slot See implementation.
 * @param[in] event See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void internal_ielsr_write(uint16_t slot, ra8_elc_event_t event)
{
  volatile uint32_t* ielsr = ra8_icu_ielsr(slot);
  if (ielsr != nullptr) { /* GCOVR_EXCL_BR_LINE -- slot bounds already validated */
    /* HUM Ch 14.2 "IELSRn : ICU Event Link Setting Register n", p 524 */
    *ielsr = (uint32_t)event & k_ra8_ielsr_iels_mask;
  }
}

/**
 * @brief Clear an IELSR slot (set event = 0, IR bit clear).
 *
 * @details See implementation.
 * @param[in] slot See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void internal_ielsr_clear(uint16_t slot)
{
  volatile uint32_t* ielsr = ra8_icu_ielsr(slot);
  if (ielsr != nullptr) { /* GCOVR_EXCL_BR_LINE -- slot bounds already validated */
    *ielsr = 0U;
  }
}

/* =============================================================================
 * Public API
 * =============================================================================
 */

ra8_err_t ra8_isr_init(void)
{
  ra8_log_info(s_tag, "ra8_isr_init");

  for (uint16_t slot = 0U; slot < k_ra8_isr_slot_count; ++slot) {
    s_slots[slot].handler  = nullptr;
    s_slots[slot].ctx      = nullptr;
    s_slots[slot].event    = (ra8_elc_event_t)0U;
    s_slots[slot].priority = 0U;
    s_slots[slot].in_use   = false;

    internal_nvic_disable(slot);
    internal_nvic_clear_pending(slot);
    internal_ielsr_clear(slot);
  }
  return k_ra8_ok;
}

/**
 * @brief Search the dispatch table for a previously-registered event.
 *
 * @return Slot index on success, ``k_ra8_isr_slot_none`` otherwise.
 *
 * @details See implementation.
 * @param[in] event See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static uint16_t internal_find_event(ra8_elc_event_t event)
{
  for (uint16_t slot = 0U; slot < k_ra8_isr_slot_count; ++slot) {
    if (s_slots[slot].in_use && s_slots[slot].event == event) {
      return slot;
    }
  }
  return k_ra8_isr_slot_none;
}

/**
 * @brief Find the first free dispatch-table slot.
 *
 * @return Slot index on success, ``k_ra8_isr_slot_none`` when full.
 *
 * @details See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static uint16_t internal_find_free(void)
{
  for (uint16_t slot = 0U; slot < k_ra8_isr_slot_count; ++slot) {
    if (!s_slots[slot].in_use) {
      return slot;
    }
  }
  return k_ra8_isr_slot_none; /* GCOVR_EXCL_LINE -- only hit if 96 slots allocated */
}

ra8_err_t ra8_isr_register(ra8_elc_event_t   event,
                           ra8_isr_handler_t handler,
                           void*             ctx,
                           uint8_t           priority,
                           uint16_t*         out_slot)
{
  RA8_CHECK_NULL_PTR(handler, s_tag, "handler must not be NULL");
  if (priority > k_ra8_isr_prio_max) {
    return k_ra8_err_invalid_arg;
  }

  const uint16_t existing = internal_find_event(event);
  if (existing != k_ra8_isr_slot_none) {
    return k_ra8_err_exists;
  }

  const uint16_t slot = internal_find_free();
  if (slot == k_ra8_isr_slot_none) { /* GCOVR_EXCL_BR_LINE -- 96-slot ceiling */
    /* GCOVR_EXCL_START */
    ra8_log_error(s_tag, "no free slot");
    return k_ra8_err_no_mem;
    /* GCOVR_EXCL_STOP */
  }

  s_slots[slot].handler  = handler;
  s_slots[slot].ctx      = ctx;
  s_slots[slot].event    = event;
  s_slots[slot].priority = priority;
  s_slots[slot].in_use   = true;

  internal_ielsr_write(slot, event);
  internal_nvic_clear_pending(slot);
  internal_nvic_set_priority(slot, priority);
  internal_nvic_enable(slot);

  if (out_slot != nullptr) {
    *out_slot = slot;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_isr_unregister(ra8_elc_event_t event)
{
  const uint16_t slot = internal_find_event(event);
  if (slot == k_ra8_isr_slot_none) {
    return k_ra8_err_not_found;
  }
  /* internal_find_event returns a slot < k_ra8_isr_slot_count or slot_none, so
   * the write below is in bounds; the explicit guard makes that provable to the
   * static analyzer (matches the bound in ra8_isr_dispatch). */
  if (slot >=
      k_ra8_isr_slot_count) {   /* GCOVR_EXCL_BR_LINE -- unreachable given the contract above */
    return k_ra8_err_not_found; /* GCOVR_EXCL_LINE                                            */
  }

  internal_nvic_disable(slot);
  internal_ielsr_clear(slot);
  internal_nvic_clear_pending(slot);

  s_slots[slot].handler  = nullptr;
  s_slots[slot].ctx      = nullptr;
  s_slots[slot].event    = (ra8_elc_event_t)0U;
  s_slots[slot].priority = 0U;
  s_slots[slot].in_use   = false;
  return k_ra8_ok;
}

void ra8_isr_dispatch(uint16_t slot)
{
  if (slot >= k_ra8_isr_slot_count) {
    return;
  }
  const ra8_isr_handler_t handler = s_slots[slot].handler;
  void* const             ctx     = s_slots[slot].ctx;

  /* Clear IELSR.IR via the accessor in ra8_icu_regs.h. The IR flag is
   * WRITE-ZERO-to-clear, not write-one-to-clear: HUM Ch 14.2.17 p 547 states
   * under "IR flag (Interrupt Status Flag)" -> [Clearing condition] that "the
   * IR flag is cleared to 0 by writing 0". ORing a 1 into the bit -- which
   * this did until issue #170 -- therefore cannot ever clear it. The ICU then
   * re-asserts the request the instant the handler returns and the core
   * live-locks in an interrupt storm that starves the main loop. Bench-proven
   * on an EK-RA8D2: lpm_periodic_idle sat in IRQ0_Handler with
   * IELSR0 = 0x00010080 (IR set) and emitted no UART at all; writing IR = 0
   * through J-Link released it and the app printed immediately.
   *
   * Read-modify-write with the IR bit masked OFF: every other field (IELS,
   * DTCE) keeps its value and only IR receives the clearing zero. */
  volatile uint32_t* ielsr = ra8_icu_ielsr(slot);
  if (ielsr != nullptr) { /* GCOVR_EXCL_BR_LINE -- validated above */
    /* HUM Ch 14.2.17 "IELSRn : Interrupt Controller Unit Event Link Setting Register n", p 546-547 */
    *ielsr = *ielsr & ~((uint32_t)1U << k_ra8_ielsr_ir_bit);
  }

  if (handler != nullptr) {
    handler(ctx);
  }
}

ra8_err_t ra8_isr_set_priority(ra8_elc_event_t event, uint8_t priority)
{
  if (priority > k_ra8_isr_prio_max) {
    return k_ra8_err_invalid_arg;
  }
  const uint16_t slot = internal_find_event(event);
  if (slot == k_ra8_isr_slot_none) {
    return k_ra8_err_not_found;
  }
  s_slots[slot].priority = priority;
  internal_nvic_set_priority(slot, priority);
  return k_ra8_ok;
}

ra8_err_t ra8_isr_lookup_slot(ra8_elc_event_t event, uint16_t* out_slot)
{
  RA8_CHECK_NULL_PTR(out_slot, s_tag, "out_slot must not be NULL");
  *out_slot = internal_find_event(event);
  return k_ra8_ok;
}

void ra8_isr_globals_enable(void)
{
  /* PRIMASK clear -- maskable IRQs may now dispatch (host no-op via seam). */
  ra8_hw_irq_enable();
}

void ra8_isr_globals_disable(void)
{
  /* PRIMASK set -- subsequent maskable IRQs pend until re-enabled (host
   * no-op via seam). */
  ra8_hw_irq_disable();
}
