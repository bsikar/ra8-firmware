/**
 * @file ra8_exception.h
 * @brief Cortex-M85 CPU exception diagnostic helpers
 * @ingroup grp_core
 *
 * @details
 * When the Cortex-M85 takes a HardFault / MemManage / BusFault /
 * UsageFault / SecureFault, the default `Default_Handler` in
 * `src/boot/vector_table.c` drops into `BKPT #0`. That is fine for
 * an attached J-Link but useless for a field deployment.
 *
 * This module provides richer fault handlers that:
 *
 *  1. Decode the stacked exception frame (`r0..r3`, `r12`, `lr`,
 *     `pc`, `xpsr`) into an `ra8_exception_frame_t` struct.
 *  2. Read the System Control Block fault-status registers
 *     (`CFSR`, `HFSR`, `DFSR`, `BFAR`, `MMFAR`, `AFSR`, plus the
 *     TrustZone `SFSR` / `SFAR` pair) and populate an
 *     `ra8_exception_diagnostics_t`.
 *  3. Emit a single log line summarising the fault.
 *  4. Call `ra8_fatal_error()` to halt.
 *
 * The same record path serves the RA8D2 NMI: the board-layer NMI
 * handler reads the ICU's NMISR cause register and forwards it here
 * via `ra8_exception_report_nmi()`, so a watchdog underflow, an SRAM
 * ECC error, or an LVD trip leaves the same debugger-readable
 * snapshot as a CPU fault instead of a bare `bkpt` trap.
 *
 * The intent is that if a field unit ever takes a HardFault, the
 * user plugs in a J-Link, attaches, and can read the stack frame +
 * CFSR bits from the log backend ring buffer instead of having to
 * reproduce the fault in a debugger session.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
 * @struct ra8_exception_frame_t
 * @brief Stacked registers pushed by the Cortex-M exception entry.
 *
 * @details
 * Layout matches the basic (non-FP) stack frame described in the Arm
 * Architecture Reference Manual section B1.5.6. The FPU variant adds
 * S0..S15 + FPSCR but we log only the core GPRs -- FPU state is rarely
 * the root cause of a fault and makes the dump three times larger.
 */
typedef struct {
  uint32_t r0;   /**< General-purpose register R0. */
  uint32_t r1;   /**< General-purpose register R1. */
  uint32_t r2;   /**< General-purpose register R2. */
  uint32_t r3;   /**< General-purpose register R3. */
  uint32_t r12;  /**< Scratch register R12.        */
  uint32_t lr;   /**< Link register (return addr). */
  uint32_t pc;   /**< Program counter at fault.    */
  uint32_t xpsr; /**< Program status register.     */
} ra8_exception_frame_t;

/**
 * @struct ra8_exception_diagnostics_t
 * @brief SCB fault-status register snapshot.
 *
 * @details
 * `sfsr` / `sfar` are the ARMv8-M Security Extension fault pair. They
 * are banked to the Secure state: read from the Secure world (where
 * every boot in this repository executes) they carry the SecureFault
 * cause bits (INVEP, INVTRAN, AUVIOL, ...) and the faulting address;
 * read from the Non-secure world they are architecturally RAZ, so the
 * unconditional capture is safe in either world.
 */
typedef struct {
  uint32_t cfsr;  /**< Configurable Fault Status Register.          */
  uint32_t hfsr;  /**< HardFault Status Register.                   */
  uint32_t dfsr;  /**< Debug Fault Status Register.                 */
  uint32_t bfar;  /**< BusFault Address Register (if BFARVALID).    */
  uint32_t mmfar; /**< MemManage Fault Address Reg (if MMARVALID).  */
  uint32_t afsr;  /**< Auxiliary Fault Status Register.             */
  uint32_t sfsr;  /**< SecureFault Status Register (RAZ from NS).   */
  uint32_t sfar;  /**< SecureFault Address Reg (if SFSR.SFARVALID). */
} ra8_exception_diagnostics_t;

/**
 * @enum ra8_exception_magic_t
 * @brief Sentinel marking a fully-written ::g_ra8_exception_last snapshot.
 *
 * @details
 * `magic` is written LAST when the snapshot is complete, so a debugger
 * (or a post-mortem reader) can distinguish a full record from a
 * half-written one interrupted by a secondary fault.
 *
 * @see g_ra8_exception_last
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_exc_magic_valid = 0xFA17DEADUL, /**< Snapshot is fully populated. */
} ra8_exception_magic_t;

/**
 * @struct ra8_exception_last_t
 * @brief Fixed-SRAM post-mortem snapshot of the most recent fault or NMI.
 *
 * @details
 * Populated unconditionally at the very top of `ra8_exception_report()`
 * BEFORE any function call that might itself fault (logging, ITM
 * access, etc.). A debugger can recover the original-fault PC, LR, SP,
 * frame pointer, SCB diagnostics, and (for NMIs) the ICU NMISR cause
 * from this struct even when the log backend is unavailable or causes
 * a secondary fault.
 *
 * @invariant `magic == k_ra8_exc_magic_valid` only after every other
 *            field has been written for the current event.
 * @invariant `nmisr` is non-zero only for records written through
 *            `ra8_exception_report_nmi()` (a hardware NMISR of zero is
 *            indistinguishable from "not an NMI" by design -- check
 *            `exc_number == 2` for the event class).
 *
 * @code
 * // Debugger / post-mortem usage:
 * //   (gdb) print g_ra8_exception_last
 * // magic 0xFA17DEAD => trust frame/diag; exc_number names the class.
 * @endcode
 *
 * @see ra8_exception_report()
 * @see ra8_exception_report_nmi()
 * @since 0.1.0
 */
typedef struct {
  uint32_t                    magic;      /**< ::k_ra8_exc_magic_valid when valid. */
  uint32_t                    exc_number; /**< Architectural exception number.     */
  ra8_exception_frame_t       frame;      /**< Stacked GPR frame.                  */
  ra8_exception_diagnostics_t diag;       /**< SCB fault-status snapshot.          */
  uintptr_t                   frame_ptr;  /**< Raw stack pointer at entry.         */
  uint32_t                    nmisr;      /**< ICU NMISR cause (NMI path only).    */
} ra8_exception_last_t;

/**
 * @var g_ra8_exception_last
 * @brief The one fixed-SRAM ::ra8_exception_last_t snapshot instance.
 *
 * @details
 * Lives at a linker-stable address so a J-Link attach after a field
 * fault can read it by symbol name with zero code running.
 *
 * @note Volatile so the compiler never elides the capture writes.
 * @warning Written only by `ra8_exception_report()`; treat as read-only
 *          everywhere else (tests read it to assert capture contents).
 * @since 0.1.0
 */
extern volatile ra8_exception_last_t g_ra8_exception_last;

/**
 * @typedef ra8_exception_persist_fn
 * @brief Post-decode persistence sink invoked once the snapshot is complete.
 *
 * @details
 * `ra8_exception_report()` calls the registered hook (if any) AFTER
 * `g_ra8_exception_last` is fully populated and its `magic` is set, but
 * BEFORE the CPU halts. It is the Dependency-Inversion seam the crash-log
 * layer (`ra8_crashlog_install()`) plugs into to copy the decoded record
 * into cross-reset `.noinit` storage. The pointer defaults to `nullptr`
 * (no persistence), so an app that never installs a hook pays nothing and
 * pulls in no crash-log code.
 *
 * @param[in] decoded The completed fixed-SRAM snapshot (never `nullptr`;
 *                    always `&g_ra8_exception_last`).
 *
 * @see ra8_exception_set_persist_hook()
 * @since 0.1.0
 */
typedef void (*ra8_exception_persist_fn)(const volatile ra8_exception_last_t* decoded);

/**
 * @brief Register (or clear) the post-decode fault-persistence hook.
 *
 * @details
 * Installs the ::ra8_exception_persist_fn that `ra8_exception_report()` calls
 * once the snapshot is complete. Passing `nullptr` disarms persistence.
 * The hook runs in fault context, so it must touch nothing that can itself
 * fault (no unpowered peripheral, no dynamic allocation) -- copying the
 * snapshot into plain SRAM is the intended use.
 *
 * @param[in] hook Persistence sink, or `nullptr` to disable.
 *
 * @return Nothing.
 *
 * @pre Called from single-threaded boot context.
 * @pre @p hook, if non-`nullptr`, is fault-context safe.
 * @post `ra8_exception_report()` invokes @p hook (or none if `nullptr`).
 * @post No fault snapshot is altered by the registration itself.
 *
 * @note Not thread-safe; arm once during boot before faults can occur.
 * @see ra8_exception_report()
 * @since 0.1.0
 */
void ra8_exception_set_persist_hook(ra8_exception_persist_fn hook);

/**
 * @brief Read the current SCB fault-status registers.
 *
 * @param[out] out Pointer to a buffer to fill. Must not be `nullptr`.
 *
 * @details See implementation for details.
 * @pre Module has been initialized.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
void ra8_exception_capture_diagnostics(ra8_exception_diagnostics_t* out);

/**
 * @brief Emit a full fault dump over the log backend.
 *
 * @details
 * Logs the exception frame + diagnostics at ERROR level, then calls
 * `ra8_fatal_error()` which halts. Meant to be called from a
 * naked assembly trampoline installed in place of the weak
 * `HardFault_Handler` alias -- see the example in `ra8_exception.c`.
 *
 * @param[in] frame       Pointer to stacked exception frame.
 * @param[in] exc_number  Exception number (2 = NMI, 3 = HardFault, ...,
 *                        7 = SecureFault).
 */
[[noreturn]] void ra8_exception_report(const ra8_exception_frame_t* frame, uint32_t exc_number);

/**
 * @brief Report an NMI with its ICU cause register, then halt.
 *
 * @details
 * The RA8D2 routes its non-maskable sources (IWDT/WDT underflow, LVD
 * voltage monitors, oscillation-stop, the NMI pin, bus / SRAM-ECC /
 * MRAM read errors, CPU lockup, FPU exception, IPC) through the ICU,
 * which latches the cause in NMISR. The board-layer `NMI_Handler`
 * reads NMISR (the register lives in the ICU, outside this core
 * module's ARM-only scope) and forwards it here. The value is staged
 * into the fixed-SRAM snapshot's `nmisr` field, logged as `nmisr=`,
 * and the common `ra8_exception_report()` path then captures the frame
 * + SCB diagnostics and halts. Exception number 2 (NMI) is implied.
 *
 * @param[in] frame  Pointer to the stacked exception frame the NMI
 *                   pushed (MSP or PSP resident); may be `nullptr`.
 * @param[in] nmisr  Raw ICU NMISR value read by the caller. Any
 *                   uint32 is accepted; bits [31:21], [19] and [11:8]
 *                   are reserved-as-zero on RA8D2.
 *
 * @return Never returns.
 *
 * @pre Invoked from the NMI handler (or a host test standing in for it).
 * @pre `g_ra8_exception_last` is writable SRAM.
 * @post `g_ra8_exception_last.nmisr` holds @p nmisr and
 *       `g_ra8_exception_last.exc_number == 2`.
 * @post Control never returns; the CPU halts at a named symbol (on the
 *       host test build, via the overridable fatal hook).
 *
 * @note Not thread-safe (single fault context by construction).
 *
 * @code
 * // Board-layer NMI handler body:
 * const uint32_t nmisr = *ra8_icu_nmisr();
 * ra8_exception_report_nmi(frame, nmisr);
 * @endcode
 *
 * @see ra8_exception_report()  The common capture + halt path.
 * @see g_ra8_exception_last    Where the cause is recorded.
 *
 * @since 0.1.0
 */
[[noreturn]] void ra8_exception_report_nmi(const ra8_exception_frame_t* frame, uint32_t nmisr);

#ifdef __cplusplus
}
#endif
