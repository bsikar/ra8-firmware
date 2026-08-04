/**
 * @file ra8_error_handler.c
 * @brief Default implementation of the fatal-error sink
 *
 * @details
 * Default implementation of `internal_ra8_fatal_error()`. This function
 * is called for every assertion failure and every failed
 * `RA8_ERROR_CHECK`. It:
 *
 *  1. Masks all maskable interrupts (`__disable_irq`).
 *  2. Logs the error via the standard `ra8_log_error_val()` path.
 *  3. Triggers a debugger breakpoint via `__BKPT(0)` so that an
 *     attached J-Link halts with the call stack intact.
 *  4. Drops into an infinite loop with `__WFI` so the CPU sleeps
 *     between interrupts (even though all maskable ones are off).
 *
 * The function is marked `__attribute__((weak))` so that field builds
 * can override it to trigger a watchdog reset or a safety-halt
 * recovery sequence without editing this file.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_error_handler.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_log.h"

/**
 * @brief Architectural `__disable_irq`: PRIMASK write.
 *
 * @details
 * Inline assembly that sets PRIMASK.PM, masking every maskable
 * interrupt at the NVIC level. Kept as a static inline here so
 * `ra8_error_handler.c` does not pull in a full CMSIS dependency -- it
 * is intentionally self-contained so a failure in CMSIS init cannot
 * prevent the fault handler from running.
 *
 * @pre None -- callable from any context, including a fault handler.
 * @pre Build is not `RA8_OFF_TARGET` (no-op on the host).
 * @post PRIMASK.PM = 1 on the target; no-op on the off-target host.
 * @post No other CPU register is touched (clobber list is `memory`).
 *
 * @note Trivially thread-safe -- single CPSID instruction.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static inline void internal_disable_irq(void)
{
#ifndef RA8_OFF_TARGET
  __asm__ volatile("cpsid i" ::: "memory");
#endif
}

/**
 * @brief Architectural `__BKPT(0)` wrapper.
 *
 * @details
 * `bkpt #0` halts execution when a debugger is attached. Without a
 * debugger the instruction faults to the HardFault handler, which is
 * acceptable -- by the time we reach this function we have already
 * decided that continuing is unsafe.
 *
 * @pre Caller has already decided the firmware cannot continue.
 * @pre Build is not `RA8_OFF_TARGET` (no-op on the host).
 * @post Execution halts under debugger; otherwise faults to HardFault.
 * @post Returns only if debugger steps over the BKPT.
 *
 * @note Single instruction; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static inline void internal_bkpt(void)
{
#ifndef RA8_OFF_TARGET
  __asm__ volatile("bkpt #0");
#endif
}

#ifndef RA8_OFF_TARGET
/**
 * @brief Architectural `__WFI` wrapper used inside the halt loop.
 *
 * @details Issues a `wfi` so the CPU drops into sleep until the next
 *          exception. Used inside the post-fault halt loop so the MCU
 *          is not burning power spinning.
 *
 *          The whole definition -- not just its body -- sits inside the
 *          on-target guard because the sole call site is the firmware
 *          halt loop below, which is itself compiled only off the fake
 *          path. A host build that kept an empty definition would carry a
 *          function nothing calls, which -Wunused-function reports.
 *
 * @pre Build is not `RA8_OFF_TARGET` (the host does not compile this).
 * @pre Called from the halt loop after IRQs have been masked.
 * @post CPU enters WFI sleep until any exception wakes it.
 * @post No register state modified.
 *
 * @note Single architectural instruction; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static inline void internal_wfi(void)
{
  __asm__ volatile("wfi");
}
#endif

/**
 * @brief Fatal-error trap: log and halt the system.
 *
 * @details Disables interrupts, emits a best-effort error line plus the
 *          numeric @p err code, then traps via BKPT and an infinite loop
 *          (or @c __builtin_trap on the off-target host so unit tests fail
 *          loudly instead of spinning forever).
 *
 * @param[in] tag     NUL-terminated module/component tag.
 * @param[in] message NUL-terminated short failure description.
 * @param[in] err     Numeric error code (typically a @c ra8_err_t value).
 *
 * @pre tag and message are non-NULL, NUL-terminated.
 * @pre Interrupt state is recoverable from disabled (we never return).
 * @post Interrupts are masked.
 * @post Function does not return; control is trapped or loops forever.
 *
 * @note Weak symbol; downstream apps may override with a richer halt path.
 *
 * @since 0.1.0
 */
[[gnu::weak]] void internal_ra8_fatal_error(const char* tag, const char* message, uint32_t err)
{
  internal_disable_irq();

  /* Best-effort log. If the log backend itself is broken we still
   * halt -- logging must never prevent the halt. */
  ra8_log_error(tag, message);
  ra8_log_error_val(tag, "err=", err);

  internal_bkpt();

#ifdef RA8_OFF_TARGET
  /* On the host we cannot actually halt; abort makes the unit test
   * runner fail loudly instead of spinning. */
  __builtin_trap();
#else
  while (1) {
    internal_wfi();
  }
#endif
}
