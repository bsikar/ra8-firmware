/**
 * @file ra_exception.c
 * @brief Cortex-M85 CPU exception diagnostic implementation
 *
 * @details
 * Reads the SCB fault status registers and logs the stacked
 * exception frame before halting via `internal_ra_fatal_error()`.
 *
 * ## Installing as the HardFault handler
 *
 * The weak `HardFault_Handler` in `src/boot/vector_table.c` is
 * replaced by a non-weak naked trampoline:
 *
 * @code{.c}
 * __attribute__((naked)) void HardFault_Handler(void)
 * {
 *     __asm__ volatile(
 *         "tst lr, #4          \n"   // MSP or PSP?
 *         "ite eq              \n"
 *         "mrseq r0, msp       \n"
 *         "mrsne r0, psp       \n"
 *         "mov r1, #3          \n"   // exception number for HardFault
 *         "b ra_exception_report\n"
 *     );
 * }
 * @endcode
 *
 * The trampoline picks the stack pointer the fault was taken on
 * (MSP if EXC_RETURN[2]=0, PSP otherwise) and tail-calls
 * `ra_exception_report()` with a pointer to the stacked frame.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_exception.h"

#include <stdint.h>

#include "ra_error_handler.h"
#include "ra_log.h"

typedef enum : uintptr_t {
  k_ra_scb_cfsr_addr  = 0xE000ED28UL,
  k_ra_scb_hfsr_addr  = 0xE000ED2CUL,
  k_ra_scb_dfsr_addr  = 0xE000ED30UL,
  k_ra_scb_mmfar_addr = 0xE000ED34UL,
  k_ra_scb_bfar_addr  = 0xE000ED38UL,
  k_ra_scb_afsr_addr  = 0xE000ED3CUL,
} ra_scb_addr_t;

/**
 * @brief Volatile 32-bit read at an absolute address.
 *
 * @details Used for SCB fault-status register reads. Wrapping the cast
 *          keeps the call sites readable.
 *
 * @param[in] addr Absolute MMIO address to read.
 *
 * @return The 32-bit value at `addr`.
 * @retval 0..UINT32_MAX  Whatever the MMIO register currently holds.
 *
 * @pre `addr` points to memory that is mapped and 4-byte aligned.
 * @pre Caller has confirmed the access is safe.
 * @post No state modified.
 * @post Behaves as a volatile load.
 *
 * @note Always inlined; trivially thread-safe.
 *
 * @since 0.1.0
 */
static inline uint32_t internal_read32(uintptr_t addr)
{
  return *(volatile uint32_t*)addr;
}

/**
 * @brief Implementation of `ra_exception_capture_diagnostics()`.
 *
 * @details Loads CFSR, HFSR, DFSR, MMFAR, BFAR, and AFSR via volatile
 *          reads. NULL argument is tolerated and returns silently.
 *
 * @param[out] out Destination buffer. May be `nullptr`.
 *
 * @return None.
 * @retval None
 *
 * @pre `out` is either `nullptr` or points to writable storage.
 * @pre SCB MMIO at 0xE000ED00 is accessible.
 * @post On non-`nullptr` input, every field of `*out` reflects the SCB
 *       at the moment of the call.
 * @post No SCB register is modified.
 *
 * @note Not reentrant w.r.t. concurrent SCB writes.
 *
 * @since 0.1.0
 */
void ra_exception_capture_diagnostics(ra_exception_diagnostics_t* out)
{
  if (out == nullptr) {
    return;
  }
  out->cfsr  = internal_read32(k_ra_scb_cfsr_addr);
  out->hfsr  = internal_read32(k_ra_scb_hfsr_addr);
  out->dfsr  = internal_read32(k_ra_scb_dfsr_addr);
  out->mmfar = internal_read32(k_ra_scb_mmfar_addr);
  out->bfar  = internal_read32(k_ra_scb_bfar_addr);
  out->afsr  = internal_read32(k_ra_scb_afsr_addr);
}

/**
 * @brief Implementation of `ra_exception_report()` -- fault dump + halt.
 *
 * @details Dumps the stacked frame and SCB diagnostics at ERROR level,
 *          then traps into `internal_ra_fatal_error()`.
 *
 * @param[in] frame      Stacked exception frame; may be `nullptr`.
 * @param[in] exc_number Architectural exception number.
 *
 * @return Does not return.
 * @retval None
 *
 * @pre Invoked from a fault context.
 * @pre Log backend is callable.
 * @post Control never returns; CPU is halted.
 * @post Diagnostic registers are emitted at ERROR level.
 *
 * @note Marked `noreturn`. Not thread-safe.
 *
 * @since 0.1.0
 */
void ra_exception_report(const ra_exception_frame_t* frame, uint32_t exc_number)
{
  ra_exception_diagnostics_t diag = {};
  ra_exception_capture_diagnostics(&diag);

  ra_log_error_val("EXC", "exception", exc_number);

  if (frame != nullptr) {
    ra_log_error_val("EXC", "pc  ", frame->pc);
    ra_log_error_val("EXC", "lr  ", frame->lr);
    ra_log_error_val("EXC", "xpsr", frame->xpsr);
    ra_log_error_val("EXC", "r0  ", frame->r0);
    ra_log_error_val("EXC", "r1  ", frame->r1);
    ra_log_error_val("EXC", "r2  ", frame->r2);
    ra_log_error_val("EXC", "r3  ", frame->r3);
    ra_log_error_val("EXC", "r12 ", frame->r12);
  }

  ra_log_error_val("EXC", "cfsr ", diag.cfsr);
  ra_log_error_val("EXC", "hfsr ", diag.hfsr);
  ra_log_error_val("EXC", "bfar ", diag.bfar);
  ra_log_error_val("EXC", "mmfar", diag.mmfar);

  internal_ra_fatal_error("EXC", "fault", exc_number);
}
