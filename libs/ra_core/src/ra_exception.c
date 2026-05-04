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

#ifdef RA_SIMULATOR_MODE
/* Host-side test builds get the legacy halt-via-fatal-error path so
 * tests can override `internal_ra_fatal_error` to longjmp out. */
#define RA_EXCEPTION_HALT(tag, msg, exc) internal_ra_fatal_error((tag), (msg), (exc))
#else
#define RA_EXCEPTION_HALT(tag, msg, exc)                                                           \
  do {                                                                                             \
    (void)(tag);                                                                                   \
    (void)(msg);                                                                                   \
    (void)(exc);                                                                                   \
    ra_exception_halt_loop();                                                                      \
  } while (0)
#endif

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
 * @var g_ra_exception_last
 * @brief Fixed-SRAM snapshot of the most recent fault.
 *
 * @details
 * Populated UNCONDITIONALLY at the very top of ra_exception_report()
 * BEFORE any function call that might itself fault (logging, ITM
 * access, etc.). A debugger can recover the original-fault PC, LR,
 * SP, frame pointer, and SCB diagnostics from this struct even when
 * the log backend is unavailable or causes a secondary fault.
 *
 * `g_ra_exception_last.magic` is set to 0xFA17DEAD only after the
 * struct is fully populated, so a half-written snapshot is detectable.
 *
 * @note Volatile so the compiler does not optimise away the writes.
 * @since 0.1.0
 */
typedef struct {
  uint32_t                   magic;      /**< 0xFA17DEAD when valid.        */
  uint32_t                   exc_number; /**< Architectural exception num.  */
  ra_exception_frame_t       frame;      /**< Stacked GPR frame.            */
  ra_exception_diagnostics_t diag;       /**< SCB fault-status snapshot.    */
  uintptr_t                  frame_ptr;  /**< Raw stack pointer at entry.   */
} ra_exception_last_t;

volatile ra_exception_last_t g_ra_exception_last;

typedef enum : uint32_t {
  k_ra_exc_magic_valid = 0xFA17DEADUL, /**< Sentinel for full snapshot.   */
} ra_exc_magic_t;

#ifndef RA_SIMULATOR_MODE
/**
 * @brief Spin halt with a known PC at a named symbol.
 *
 * @details
 * The fault handler MUST terminate at a symbol the debugger can name
 * rather than escalate to LOCKUP at PC=0xEFFFFFFE. This function is a
 * `wfi` loop in its own translation-unit-local symbol so a backtrace
 * unambiguously points at "we got here from the fault handler" rather
 * than at a random unmapped address.
 *
 * @pre All maskable interrupts have been disabled by the caller.
 * @pre g_ra_exception_last has been populated with the fault snapshot.
 * @post Function never returns.
 * @post CPU is parked in a wfi loop with IRQs masked.
 *
 * @note `noreturn`. Trivially thread-safe.
 *
 * @since 0.1.0
 */
__attribute__((noreturn, noinline)) static void ra_exception_halt_loop(void)
{
  __asm__ volatile("cpsid i" ::: "memory");
  while (1) {
    __asm__ volatile("wfi");
  }
}
#endif

/**
 * @brief Implementation of `ra_exception_report()` -- fault dump + halt.
 *
 * @details
 * Captures the stacked frame and SCB diagnostics into the fixed-SRAM
 * snapshot `g_ra_exception_last` BEFORE any function call that might
 * itself fault. Then best-effort logs them via `ra_log_error_val`
 * (which now silently drops every byte from a fault context, see
 * libs/ra_core/src/ra_log.c). Finally parks the CPU at the named
 * `ra_exception_halt_loop` symbol on target so the debugger can give
 * the halt a clean backtrace instead of escalating to LOCKUP at
 * PC=0xEFFFFFFE.
 *
 * @param[in] frame      Stacked exception frame; may be `nullptr`.
 * @param[in] exc_number Architectural exception number.
 *
 * @pre Invoked from a fault context (IPSR != 0).
 * @pre `g_ra_exception_last` is writable SRAM.
 * @post `g_ra_exception_last.magic == 0xFA17DEAD` once snapshot is complete.
 * @post Control never returns; CPU is halted at a named symbol.
 *
 * @note Marked `noreturn`. Not thread-safe.
 *
 * @since 0.1.0
 */
void ra_exception_report(const ra_exception_frame_t* frame, uint32_t exc_number)
{
  /* Step 1: capture EVERYTHING into fixed SRAM FIRST -- before any
   * function call that might itself fault. This guarantees that even
   * if the log backend, ITM, or anything else takes a secondary fault,
   * a debugger can still recover the original fault context from
   * g_ra_exception_last. */
  g_ra_exception_last.magic      = 0U;
  g_ra_exception_last.exc_number = exc_number;
  g_ra_exception_last.frame_ptr  = (uintptr_t)frame;
  if (frame != nullptr) {
    g_ra_exception_last.frame.r0   = frame->r0;
    g_ra_exception_last.frame.r1   = frame->r1;
    g_ra_exception_last.frame.r2   = frame->r2;
    g_ra_exception_last.frame.r3   = frame->r3;
    g_ra_exception_last.frame.r12  = frame->r12;
    g_ra_exception_last.frame.lr   = frame->lr;
    g_ra_exception_last.frame.pc   = frame->pc;
    g_ra_exception_last.frame.xpsr = frame->xpsr;
  }
  ra_exception_diagnostics_t diag = {};
  ra_exception_capture_diagnostics(&diag);
  g_ra_exception_last.diag  = diag;
  g_ra_exception_last.magic = (uint32_t)k_ra_exc_magic_valid;

  /* Step 2: best-effort logging. The logging backend (ra_log) now
   * gates ITM access on DEMCR.TRCENA and IPSR; in fault context this
   * silently drops every byte instead of bus-faulting. */
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

  /* Step 3: halt at a named symbol. On target we do NOT call
   * internal_ra_fatal_error from a fault context -- that path issues
   * `bkpt #0`, which on a board without an attached debugger re-enters
   * HardFault and can escalate to LOCKUP at PC=0xEFFFFFFE. The
   * dedicated halt loop parks PC at a symbol the debugger can name.
   * On host (simulator) builds we still go through the overridable
   * fatal hook so unit tests can longjmp out. */
  RA_EXCEPTION_HALT("EXC", "fault", exc_number);
}
