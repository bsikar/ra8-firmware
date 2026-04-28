/**
 * @file ra_isr.c
 * @brief NVIC + ICU IELSR allocator implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * substrate. Owns every write to the ICU IELSR slot array
 * and to the Cortex-M85 NVIC ISER / ICER / IPR windows. See
 * ``ra_isr.h`` for the API contract.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_isr.h"

#include <stdint.h>

#include "ra8d2_icu_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

static const char* s_tag = "ISR";

/**
 * @enum ra_isr_nvic_t
 * @brief NVIC register-base offsets used by the dispatcher.
 *
 * @details
 * Cortex-M85 architectural layout from the Armv8.1-M reference
 * manual. The same addresses apply on the Cortex-M33 secondary
 * core; that is why this module is a substrate module and not a
 * target-specific peripheral driver.
 */
typedef enum : uintptr_t {
  k_ra_isr_nvic_iser_base = 0xE000E100UL, /**< ISER base: enables. */
  k_ra_isr_nvic_icer_base = 0xE000E180UL, /**< ICER base: disables. */
  k_ra_isr_nvic_icpr_base = 0xE000E280UL, /**< ICPR base: clear pending. */
  k_ra_isr_nvic_ipr_base  = 0xE000E400UL, /**< IPR base: priority bytes. */
} ra_isr_nvic_t;

/**
 * @enum ra_isr_nvic_layout_t
 * @brief Bit-per-word geometry of ISER / ICER arrays.
 */
typedef enum : uint16_t {
  k_ra_isr_nvic_bits_per_word = 32U, /**< 32 bits per ISER/ICER entry. */
  k_ra_isr_nvic_prio_shift    = 4U,  /**< Upper 4 priority bits used. */
} ra_isr_nvic_layout_t;

/**
 * @struct ra_isr_slot_t
 * @brief Dispatch-table entry for one IELSR slot.
 */
typedef struct {
  ra_isr_handler_t handler;  /**< Driver callback, NULL if free. */
  void*            ctx;      /**< Context passed to handler. */
  ra_elc_event_t   event;    /**< Event mapped to this slot. */
  uint8_t          priority; /**< Last-set NVIC priority. */
  bool             in_use;   /**< True when allocated. */
} ra_isr_slot_t;

/**
 * @var s_slots
 * @brief Per-slot dispatch table.
 *
 * @details
 * Indexed by slot number 0..k_ra_isr_slot_count - 1. Updated by
 * ``ra_isr_register / unregister`` and read by ``ra_isr_dispatch``.
 * Static so it is zero-initialised at boot.
 */
static ra_isr_slot_t s_slots[k_ra_isr_slot_count];

/* =============================================================================
 * NVIC pokes (no-op on host)
 * =============================================================================
 *
 * The simulator maps the SCB / NVIC window via ra_sim_mmap (core
 * region base 0xE0000000, size 0x100000). Writes land in host
 * RAM; the tests observe the register state via the same aliases.
 */

/**
 * @brief Enable NVIC line ``n``.
 */
static void internal_nvic_enable(uint16_t n)
{
  const uint16_t     word = n / (uint16_t)k_ra_isr_nvic_bits_per_word;
  const uint16_t     bit  = n % (uint16_t)k_ra_isr_nvic_bits_per_word;
  volatile uint32_t* iser =
    (volatile uint32_t*)(k_ra_isr_nvic_iser_base + ((uintptr_t)word * sizeof(uint32_t)));
  *iser = (uint32_t)1UL << bit;
}

/**
 * @brief Disable NVIC line ``n``.
 */
static void internal_nvic_disable(uint16_t n)
{
  const uint16_t     word = n / (uint16_t)k_ra_isr_nvic_bits_per_word;
  const uint16_t     bit  = n % (uint16_t)k_ra_isr_nvic_bits_per_word;
  volatile uint32_t* icer =
    (volatile uint32_t*)(k_ra_isr_nvic_icer_base + ((uintptr_t)word * sizeof(uint32_t)));
  *icer = (uint32_t)1UL << bit;
}

/**
 * @brief Clear pending flag on NVIC line ``n``.
 */
static void internal_nvic_clear_pending(uint16_t n)
{
  const uint16_t     word = n / (uint16_t)k_ra_isr_nvic_bits_per_word;
  const uint16_t     bit  = n % (uint16_t)k_ra_isr_nvic_bits_per_word;
  volatile uint32_t* icpr =
    (volatile uint32_t*)(k_ra_isr_nvic_icpr_base + ((uintptr_t)word * sizeof(uint32_t)));
  *icpr = (uint32_t)1UL << bit;
}

/**
 * @brief Set NVIC priority byte for line ``n``.
 */
static void internal_nvic_set_priority(uint16_t n, uint8_t prio)
{
  volatile uint8_t* ipr = (volatile uint8_t*)(k_ra_isr_nvic_ipr_base + (uintptr_t)n);
  *ipr                  = (uint8_t)(prio << k_ra_isr_nvic_prio_shift);
}

/**
 * @brief Write the event number into an IELSR slot.
 *
 * @details
 * The 9-bit event number goes into bits 8..0 per HUM Ch 14.2.x
 * (IELSRn layout). Upper bits (DTC enable, IR status) are written
 * zero so the slot starts clean.
 */
static void internal_ielsr_write(uint16_t slot, ra_elc_event_t event)
{
  volatile uint32_t* ielsr = ra_icu_ielsr(slot);
  if (ielsr != nullptr) { /* GCOVR_EXCL_BR_LINE -- slot bounds already validated */
    /* HUM Ch 14.2 "IELSRn : ICU Event Link Setting Register n", p 524 */
    *ielsr = (uint32_t)event & (uint32_t)k_ra_ielsr_iels_mask;
  }
}

/**
 * @brief Clear an IELSR slot (set event = 0, IR bit clear).
 */
static void internal_ielsr_clear(uint16_t slot)
{
  volatile uint32_t* ielsr = ra_icu_ielsr(slot);
  if (ielsr != nullptr) { /* GCOVR_EXCL_BR_LINE -- slot bounds already validated */
    *ielsr = 0U;
  }
}

/* =============================================================================
 * Public API
 * =============================================================================
 */

ra_err_t ra_isr_init(void)
{
  ra_log_info(s_tag, "ra_isr_init");

  for (uint16_t slot = 0U; slot < (uint16_t)k_ra_isr_slot_count; ++slot) {
    s_slots[slot].handler  = nullptr;
    s_slots[slot].ctx      = nullptr;
    s_slots[slot].event    = (ra_elc_event_t)0U;
    s_slots[slot].priority = 0U;
    s_slots[slot].in_use   = false;

    internal_nvic_disable(slot);
    internal_nvic_clear_pending(slot);
    internal_ielsr_clear(slot);
  }
  return k_ra_ok;
}

/**
 * @brief Search the dispatch table for a previously-registered event.
 *
 * @return Slot index on success, ``k_ra_isr_slot_none`` otherwise.
 */
static uint16_t internal_find_event(ra_elc_event_t event)
{
  for (uint16_t slot = 0U; slot < (uint16_t)k_ra_isr_slot_count; ++slot) {
    if (s_slots[slot].in_use && s_slots[slot].event == event) {
      return slot;
    }
  }
  return (uint16_t)k_ra_isr_slot_none;
}

/**
 * @brief Find the first free dispatch-table slot.
 *
 * @return Slot index on success, ``k_ra_isr_slot_none`` when full.
 */
static uint16_t internal_find_free(void)
{
  for (uint16_t slot = 0U; slot < (uint16_t)k_ra_isr_slot_count; ++slot) {
    if (!s_slots[slot].in_use) {
      return slot;
    }
  }
  return (uint16_t)k_ra_isr_slot_none; /* GCOVR_EXCL_LINE -- only hit if 96 slots allocated */
}

ra_err_t ra_isr_register(ra_elc_event_t   event,
                         ra_isr_handler_t handler,
                         void*            ctx,
                         uint8_t          priority,
                         uint16_t*        out_slot)
{
  RA_CHECK_NULL_PTR((void*)handler, s_tag, "handler must not be NULL");
  if (priority > (uint8_t)k_ra_isr_prio_max) {
    return k_ra_err_invalid_arg;
  }

  const uint16_t existing = internal_find_event(event);
  if (existing != (uint16_t)k_ra_isr_slot_none) {
    return k_ra_err_exists;
  }

  const uint16_t slot = internal_find_free();
  if (slot == (uint16_t)k_ra_isr_slot_none) { /* GCOVR_EXCL_BR_LINE -- 96-slot ceiling */
    /* GCOVR_EXCL_START */
    ra_log_error(s_tag, "no free slot");
    return k_ra_err_no_mem;
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
  return k_ra_ok;
}

ra_err_t ra_isr_unregister(ra_elc_event_t event)
{
  const uint16_t slot = internal_find_event(event);
  if (slot == (uint16_t)k_ra_isr_slot_none) {
    return k_ra_err_not_found;
  }

  internal_nvic_disable(slot);
  internal_ielsr_clear(slot);
  internal_nvic_clear_pending(slot);

  s_slots[slot].handler  = nullptr;
  s_slots[slot].ctx      = nullptr;
  s_slots[slot].event    = (ra_elc_event_t)0U;
  s_slots[slot].priority = 0U;
  s_slots[slot].in_use   = false;
  return k_ra_ok;
}

void ra_isr_dispatch(uint16_t slot)
{
  if (slot >= (uint16_t)k_ra_isr_slot_count) {
    return;
  }
  const ra_isr_handler_t handler = s_slots[slot].handler;
  void* const            ctx     = s_slots[slot].ctx;

  /* Clear IELSR.IR via the accessor in ra8d2_icu_regs.h. Writing
   * the IR bit follows the HUM 14.2 "write-one-to-clear" rule. */
  volatile uint32_t* ielsr = ra_icu_ielsr(slot);
  if (ielsr != nullptr) { /* GCOVR_EXCL_BR_LINE -- validated above */
    /* HUM Ch 14.2 "IELSRn : ICU Event Link Setting Register n", p 524 */
    *ielsr = *ielsr | ((uint32_t)1U << (uint8_t)k_ra_ielsr_ir_bit);
  }

  if (handler != nullptr) {
    handler(ctx);
  }
}

ra_err_t ra_isr_set_priority(ra_elc_event_t event, uint8_t priority)
{
  if (priority > (uint8_t)k_ra_isr_prio_max) {
    return k_ra_err_invalid_arg;
  }
  const uint16_t slot = internal_find_event(event);
  if (slot == (uint16_t)k_ra_isr_slot_none) {
    return k_ra_err_not_found;
  }
  s_slots[slot].priority = priority;
  internal_nvic_set_priority(slot, priority);
  return k_ra_ok;
}

ra_err_t ra_isr_lookup_slot(ra_elc_event_t event, uint16_t* out_slot)
{
  RA_CHECK_NULL_PTR(out_slot, s_tag, "out_slot must not be NULL");
  *out_slot = internal_find_event(event);
  return k_ra_ok;
}
