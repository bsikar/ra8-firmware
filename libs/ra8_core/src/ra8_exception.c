/**
 * @file ra8_exception.c
 * @brief Cortex-M85 CPU exception diagnostic implementation
 *
 * @details
 * Reads the SCB fault status registers and logs the stacked
 * exception frame before halting via `internal_ra8_fatal_error()`.
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
 *         "b ra8_exception_report\n"
 *     );
 * }
 * @endcode
 *
 * The trampoline picks the stack pointer the fault was taken on
 * (MSP if EXC_RETURN[2]=0, PSP otherwise) and tail-calls
 * `ra8_exception_report()` with a pointer to the stacked frame.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_exception.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_error_handler.h"
#include "ra8_log.h"
#include "ra8_scb.h"

#ifdef RA8_OFF_TARGET
/* Host-side test builds get the legacy halt-via-fatal-error path so
 * tests can override `internal_ra8_fatal_error` to longjmp out. */
/** @brief RA8 EXCEPTION HALT. */
#define RA8_EXCEPTION_HALT(tag, msg, exc) internal_ra8_fatal_error((tag), (msg), (exc))
#else
/** @brief RA8 EXCEPTION HALT. */
#define RA8_EXCEPTION_HALT(tag, msg, exc)                                                          \
  do {                                                                                             \
    (void)(tag);                                                                                   \
    (void)(msg);                                                                                   \
    (void)(exc);                                                                                   \
    ra8_exception_halt_loop();                                                                     \
  } while (0)
#endif

/**
 * @enum ra8_exc_num_t
 * @brief Architectural exception numbers this module special-cases.
 */
typedef enum : uint32_t {
  k_ra8_exc_num_nmi = 2U, /**< NMI vector slot -- carries an ICU NMISR cause. */
} ra8_exc_num_t;

/**
 * @brief Implementation of `ra8_exception_capture_diagnostics()` -- routes the
 *        SCB fault-status read through the shared ra8_scb primitive.
 *
 * @details Delegates the CFSR / HFSR / DFSR / MMFAR / BFAR / AFSR + Secure
 *          SFSR / SFAR reads to ::ra8_scb_read_fault_status, then copies the
 *          snapshot into the exception-record layout. NULL argument is
 *          tolerated and returns silently (the fault path must never log from
 *          here), so the shared primitive is only ever handed a stack local
 *          and its null-guard return cannot trip. Behaviourally identical to
 *          the previous inline reads: the same eight registers in the same
 *          order, no register written.
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
void ra8_exception_capture_diagnostics(ra8_exception_diagnostics_t* out)
{
  if (out == nullptr) {
    return;
  }
  ra8_scb_fault_status_t fs = {};
  if (ra8_scb_read_fault_status(&fs) == k_ra8_ok) {
    out->cfsr  = fs.cfsr;
    out->hfsr  = fs.hfsr;
    out->dfsr  = fs.dfsr;
    out->mmfar = fs.mmfar;
    out->bfar  = fs.bfar;
    out->afsr  = fs.afsr;
    out->sfsr  = fs.sfsr;
    out->sfar  = fs.sfar;
  }
}

/** @brief Definition of `g_ra8_exception_last` -- contract documented in ra8_exception.h. */
volatile ra8_exception_last_t g_ra8_exception_last;

/**
 * @var s_ra8_exception_nmi_stage
 * @brief NMISR cause staged by `ra8_exception_report_nmi()` for the record.
 *
 * @details
 * `ra8_exception_report()` owns the fixed-SRAM snapshot write order
 * (everything first, `magic` last). The NMI entry point cannot write
 * the record directly without racing that ordering, so it stages the
 * ICU cause here (a plain SRAM store that cannot fault) and the common
 * path copies it into `g_ra8_exception_last.nmisr` -- then clears the
 * stage so a later non-NMI record never inherits a stale cause.
 *
 * @note Written only by `ra8_exception_report_nmi()`; consumed and
 *       cleared by `ra8_exception_report()`.
 * @warning Do not read outside this translation unit.
 * @since 0.1.0
 */
static volatile uint32_t s_ra8_exception_nmi_stage;

/**
 * @var s_ra8_exception_persist
 * @brief Registered post-decode persistence sink, or `nullptr` when disarmed.
 *
 * @details
 * Set by `ra8_exception_set_persist_hook()` and invoked by
 * `ra8_exception_report()` once the fixed-SRAM snapshot is complete. Lives
 * in `.bss` (zeroed by every reset), so a consumer must re-arm it early on
 * each boot. Left `nullptr` unless an app opts into fault persistence, so
 * the default fault path pulls in no crash-log code.
 *
 * @note Written only by `ra8_exception_set_persist_hook()`; read only by
 *       `ra8_exception_report()`.
 * @warning Do not read or write outside this translation unit.
 * @since 0.1.0
 */
static ra8_exception_persist_fn s_ra8_exception_persist;

/** @brief Implementation of `ra8_exception_set_persist_hook()` -- store the sink pointer. */
void ra8_exception_set_persist_hook(ra8_exception_persist_fn hook)
{
  s_ra8_exception_persist = hook;
}

#ifndef RA8_OFF_TARGET
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
 * @pre g_ra8_exception_last has been populated with the fault snapshot.
 * @post Function never returns.
 * @post CPU is parked in a wfi loop with IRQs masked.
 *
 * @note `noreturn`. Trivially thread-safe.
 *
 * @since 0.1.0
 */
[[noreturn, gnu::noinline]] static void ra8_exception_halt_loop(void)
{
  __asm__ volatile("cpsid i" ::: "memory");
  while (1) {
    __asm__ volatile("wfi");
  }
}
#endif

/**
 * @brief Best-effort log dump of a captured fault record.
 *
 * @details
 * Emits the exception number, the stacked frame (when present), the
 * SCB diagnostics, and -- for the NMI class only -- the ICU NMISR
 * cause, all via `ra8_log_error_val`. Runs strictly AFTER the fixed-SRAM
 * snapshot is complete, so a secondary fault inside the log backend
 * can no longer lose the record.
 *
 * @param[in] frame      Stacked exception frame; may be `nullptr`.
 * @param[in] exc_number Architectural exception number.
 * @param[in] diag       Captured SCB diagnostics (never `nullptr`;
 *                       sole caller passes a stack local).
 * @param[in] nmisr      Recorded ICU NMI cause (logged only for exc 2).
 *
 * @pre `g_ra8_exception_last.magic` is already ::k_ra8_exc_magic_valid.
 * @pre `diag` points at the diagnostics captured for this event.
 * @post Every field above was offered to the log backend (which may
 *       drop bytes in fault context -- see ra8_log.c).
 * @post No snapshot state is modified.
 *
 * @note Not thread-safe (single fault context by construction).
 *
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_log_fault_dump(const ra8_exception_frame_t*       frame,
                                                 uint32_t                           exc_number,
                                                 const ra8_exception_diagnostics_t* diag,
                                                 uint32_t                           nmisr)
{
  ra8_log_error_val("EXC", "exception", exc_number);

  if (frame != nullptr) {
    ra8_log_error_val("EXC", "pc  ", frame->pc);
    ra8_log_error_val("EXC", "lr  ", frame->lr);
    ra8_log_error_val("EXC", "xpsr", frame->xpsr);
    ra8_log_error_val("EXC", "r0  ", frame->r0);
    ra8_log_error_val("EXC", "r1  ", frame->r1);
    ra8_log_error_val("EXC", "r2  ", frame->r2);
    ra8_log_error_val("EXC", "r3  ", frame->r3);
    ra8_log_error_val("EXC", "r12 ", frame->r12);
  }

  ra8_log_error_val("EXC", "cfsr ", diag->cfsr);
  ra8_log_error_val("EXC", "hfsr ", diag->hfsr);
  ra8_log_error_val("EXC", "bfar ", diag->bfar);
  ra8_log_error_val("EXC", "mmfar", diag->mmfar);
  ra8_log_error_val("EXC", "sfsr ", diag->sfsr);
  ra8_log_error_val("EXC", "sfar ", diag->sfar);
  if (exc_number == (uint32_t)k_ra8_exc_num_nmi) {
    ra8_log_error_val("EXC", "nmisr", nmisr);
  }
}

/**
 * @brief Implementation of `ra8_exception_report()` -- fault dump + halt.
 *
 * @details
 * Captures the stacked frame and SCB diagnostics into the fixed-SRAM
 * snapshot `g_ra8_exception_last` BEFORE any function call that might
 * itself fault. Then best-effort logs them via `internal_log_fault_dump`
 * (ra8_log silently drops every byte from a fault context on the default
 * ITM backend; a registered byte sink still emits, see
 * libs/ra8_core/src/ra8_log.c). Finally parks the CPU at the named
 * `ra8_exception_halt_loop` symbol on target so the debugger can give
 * the halt a clean backtrace instead of escalating to LOCKUP at
 * PC=0xEFFFFFFE.
 *
 * @param[in] frame      Stacked exception frame; may be `nullptr`.
 * @param[in] exc_number Architectural exception number.
 *
 * @pre Invoked from a fault context (IPSR != 0).
 * @pre `g_ra8_exception_last` is writable SRAM.
 * @post `g_ra8_exception_last.magic == 0xFA17DEAD` once snapshot is complete.
 * @post Control never returns; CPU is halted at a named symbol.
 *
 * @note Marked `noreturn`. Not thread-safe.
 *
 * @since 0.1.0
 */
void ra8_exception_report(const ra8_exception_frame_t* frame, uint32_t exc_number)
{
  /* Step 1: capture EVERYTHING into fixed SRAM FIRST -- before any
   * function call that might itself fault. This guarantees that even
   * if the log backend, ITM, or anything else takes a secondary fault,
   * a debugger can still recover the original fault context from
   * g_ra8_exception_last. */
  g_ra8_exception_last.magic      = 0U;
  g_ra8_exception_last.exc_number = exc_number;
  g_ra8_exception_last.frame_ptr  = (uintptr_t)frame;
  if (frame != nullptr) {
    g_ra8_exception_last.frame.r0   = frame->r0;
    g_ra8_exception_last.frame.r1   = frame->r1;
    g_ra8_exception_last.frame.r2   = frame->r2;
    g_ra8_exception_last.frame.r3   = frame->r3;
    g_ra8_exception_last.frame.r12  = frame->r12;
    g_ra8_exception_last.frame.lr   = frame->lr;
    g_ra8_exception_last.frame.pc   = frame->pc;
    g_ra8_exception_last.frame.xpsr = frame->xpsr;
  }
  ra8_exception_diagnostics_t diag = {};
  ra8_exception_capture_diagnostics(&diag);
  g_ra8_exception_last.diag = diag;
  /* Fold in the ICU cause staged by ra8_exception_report_nmi(), then
   * clear the stage so a later non-NMI record cannot inherit it. */
  g_ra8_exception_last.nmisr = s_ra8_exception_nmi_stage;
  s_ra8_exception_nmi_stage  = 0U;
  g_ra8_exception_last.magic = (uint32_t)k_ra8_exc_magic_valid;

  /* Step 1b: persist the completed snapshot across the coming reset (if a
   * sink is armed). Runs before any code that might take a secondary fault
   * so a field unit's post-mortem + reset-loop count survive to the next
   * boot. A plain SRAM copy -- fault-context safe. See ra8_crashlog.c. */
  if (s_ra8_exception_persist != nullptr) {
    s_ra8_exception_persist(&g_ra8_exception_last);
  }

  /* Step 2: best-effort logging, strictly after the snapshot. */
  internal_log_fault_dump(frame, exc_number, &diag, g_ra8_exception_last.nmisr);

  /* Step 3: halt at a named symbol. On target we do NOT call
   * internal_ra8_fatal_error from a fault context -- that path issues
   * `bkpt #0`, which on a board without an attached debugger re-enters
   * HardFault and can escalate to LOCKUP at PC=0xEFFFFFFE. The
   * dedicated halt loop parks PC at a symbol the debugger can name.
   * On host (fake) builds we still go through the overridable
   * fatal hook so unit tests can longjmp out. */
  RA8_EXCEPTION_HALT("EXC", "fault", exc_number);
}

/** @brief Implementation of `ra8_exception_report_nmi()` -- stage the ICU cause, then chain. */
void ra8_exception_report_nmi(const ra8_exception_frame_t* frame, uint32_t nmisr)
{
  /* Plain SRAM store first (cannot fault), so even a secondary fault
   * inside the common path leaves the cause recoverable: the stage is
   * copied into g_ra8_exception_last.nmisr before magic is set. */
  s_ra8_exception_nmi_stage = nmisr;
  ra8_exception_report(frame, (uint32_t)k_ra8_exc_num_nmi);
}
