/**
 * @file er_ns_nsc_guard.h
 * @brief Non-Secure SysTick guard around NSC calls from ThreadX PSP threads.
 *
 * @par Tag
 * [Ring 6 / APP] {World: NS}
 *
 * @details
 * On ARMv8-M, if a Non-Secure interrupt (such as ``SysTick_NS``) preempts
 * Secure Thread mode during an NSC veneer called from a ThreadX task running
 * on ``PSP_NS`` (``CONTROL_NS.SPSEL = 1``), exception entry clears
 * ``CONTROL_NS.SPSEL`` in the Non-Secure bank while ``EXC_RETURN`` records
 * only ``CONTROL_S.SPSEL`` (= 0). When ``BXNS`` returns to the caller, the
 * thread resumes on ``MSP_NS`` instead of its ``PSP_NS`` stack. Masking
 * ``SYST_CSR_NS.TICKINT`` (after draining any already-pending ``SysTick_NS``
 * while still in Non-Secure Thread mode) prevents ``SysTick_NS`` from firing
 * inside the Secure window while leaving the SysTick reload counter running.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#if !defined(RA8_OFF_TARGET) && defined(__ARM_ARCH)

typedef enum : uintptr_t {
  k_er_ns_syst_csr_addr = 0xE000E010UL, /**< NS SysTick SYST_CSR. */
  k_er_ns_scb_icsr_addr = 0xE000ED04UL, /**< NS SCB ICSR.         */
} er_ns_nsc_guard_addr_t;

typedef enum : uint32_t {
  k_er_ns_syst_tickint_mask = 1UL << 1U,  /**< SYST_CSR.TICKINT bit. */
  k_er_ns_icsr_pendstset    = 1UL << 26U, /**< ICSR.PENDSTSET bit.   */
  k_er_ns_primask_pm        = 1UL << 0U,  /**< Armv8-M PRIMASK.PM.   */
} er_ns_nsc_guard_bits_t;

/**
 * @brief Mask ``SysTick_NS`` delivery across an NSC call from NS Thread mode.
 *
 * @details Clears ``SYST_CSR_NS.TICKINT`` atomically under ``PRIMASK_NS`` and
 *          drains any ``SysTick_NS`` already pended in ``ICSR.PENDSTSET`` while
 *          still in Non-Secure Thread mode (when ``PRIMASK_NS`` was clear) so
 *          no tick can preempt inside the upcoming Secure gateway window.
 *
 * @return Saved ``SYST_CSR_NS.TICKINT`` bit (``k_er_ns_syst_tickint_mask`` or
 *         ``0U``) to pass back to ::er_ns_nsc_exit.
 */
static inline uint32_t er_ns_nsc_enter(void)
{
  /* ARMv8-M ARM B3.2.2 System timer (SYST_CSR_NS) and SCB (ICSR_NS) aliases. */
  volatile uint32_t* const syst_csr = (volatile uint32_t*)(uintptr_t)k_er_ns_syst_csr_addr;
  volatile uint32_t* const icsr     = (volatile uint32_t*)(uintptr_t)k_er_ns_scb_icsr_addr;

  for (;;) {
    uint32_t primask = 0U;
    __asm__ volatile("mrs %0, primask\n\tcpsid i" : "=r"(primask)::"memory");

    const uint32_t was_tickint = (*syst_csr) & (uint32_t)k_er_ns_syst_tickint_mask;
    if (was_tickint == 0U) {
      __asm__ volatile("msr primask, %0" ::"r"(primask) : "memory");
      return 0U;
    }

    *syst_csr &= ~(uint32_t)k_er_ns_syst_tickint_mask;
    __asm__ volatile("dsb\n\tisb" ::: "memory");

    if (((*icsr & (uint32_t)k_er_ns_icsr_pendstset) == 0U) ||
        ((primask & (uint32_t)k_er_ns_primask_pm) != 0U)) {
      __asm__ volatile("msr primask, %0" ::"r"(primask) : "memory");
      return (uint32_t)k_er_ns_syst_tickint_mask;
    }

    /* A tick pended right before TICKINT cleared: service it in NS mode. */
    *syst_csr |= (uint32_t)k_er_ns_syst_tickint_mask;
    __asm__ volatile("msr primask, %0\n\tisb" ::"r"(primask) : "memory");
  }
}

/**
 * @brief Restore ``SysTick_NS`` delivery after returning from an NSC call.
 *
 * @param[in] saved_tickint Token returned by ::er_ns_nsc_enter.
 */
static inline void er_ns_nsc_exit(uint32_t saved_tickint)
{
  if (saved_tickint == 0U) {
    return;
  }

  /* ARMv8-M ARM B3.2.2 SYST_CSR_NS alias. */
  volatile uint32_t* const syst_csr = (volatile uint32_t*)(uintptr_t)k_er_ns_syst_csr_addr;
  uint32_t                 primask  = 0U;
  __asm__ volatile("mrs %0, primask\n\tcpsid i" : "=r"(primask)::"memory");
  *syst_csr |= (saved_tickint & (uint32_t)k_er_ns_syst_tickint_mask);
  __asm__ volatile("dsb\n\tisb" ::: "memory");
  __asm__ volatile("msr primask, %0" ::"r"(primask) : "memory");
}

#else

static inline uint32_t er_ns_nsc_enter(void)
{
  return 0U;
}

static inline void er_ns_nsc_exit(uint32_t saved_tickint)
{
  (void)saved_tickint;
}

#endif

/** @brief Invoke a value-returning NSC veneer with ``SysTick_NS`` masked. */
#define ER_NS_NSC_CALL(out_ret, expr)                                                              \
  do {                                                                                             \
    const uint32_t _er_nsc_guard = er_ns_nsc_enter();                                              \
    (out_ret)                    = (expr);                                                         \
    er_ns_nsc_exit(_er_nsc_guard);                                                                 \
  } while (0)

/** @brief Invoke a void NSC veneer with ``SysTick_NS`` masked. */
#define ER_NS_NSC_VOID(expr)                                                                       \
  do {                                                                                             \
    const uint32_t _er_nsc_guard = er_ns_nsc_enter();                                              \
    (expr);                                                                                        \
    er_ns_nsc_exit(_er_nsc_guard);                                                                 \
  } while (0)
