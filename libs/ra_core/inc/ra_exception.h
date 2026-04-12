/**
 * @file ra_exception.h
 * @brief Cortex-M85 CPU exception diagnostic helpers
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
 *     `pc`, `xpsr`) into an `ra_exception_frame_t` struct.
 *  2. Read the System Control Block fault-status registers
 *     (`CFSR`, `HFSR`, `DFSR`, `BFAR`, `MMFAR`, `AFSR`) and
 *     populate an `ra_exception_diagnostics_t`.
 *  3. Emit a single log line summarising the fault.
 *  4. Call `internal_ra_fatal_error()` to halt.
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
 * @struct ra_exception_frame_t
 * @brief Stacked registers pushed by the Cortex-M exception entry.
 *
 * @details
 * Layout matches the basic (non-FP) stack frame described in the Arm
 * Architecture Reference Manual section B1.5.6. The FPU variant adds
 * S0..S15 + FPSCR but we log only the core GPRs -- FPU state is rarely
 * the root cause of a fault and makes the dump three times larger.
 */
typedef struct {
  uint32_t r0;   /**< General-purpose register R0.   */
  uint32_t r1;   /**< General-purpose register R1.   */
  uint32_t r2;   /**< General-purpose register R2.   */
  uint32_t r3;   /**< General-purpose register R3.   */
  uint32_t r12;  /**< Scratch register R12.          */
  uint32_t lr;   /**< Link register (return addr).   */
  uint32_t pc;   /**< Program counter at fault.      */
  uint32_t xpsr; /**< Program status register.       */
} ra_exception_frame_t;

/**
 * @struct ra_exception_diagnostics_t
 * @brief SCB fault-status register snapshot.
 */
typedef struct {
  uint32_t cfsr;  /**< Configurable Fault Status Register.          */
  uint32_t hfsr;  /**< HardFault Status Register.                   */
  uint32_t dfsr;  /**< Debug Fault Status Register.                 */
  uint32_t bfar;  /**< BusFault Address Register (if BFARVALID).    */
  uint32_t mmfar; /**< MemManage Fault Address Reg (if MMARVALID).  */
  uint32_t afsr;  /**< Auxiliary Fault Status Register.             */
} ra_exception_diagnostics_t;

/**
 * @brief Read the current SCB fault-status registers.
 *
 * @param[out] out Pointer to a buffer to fill. Must not be `nullptr`.
 */
void ra_exception_capture_diagnostics(ra_exception_diagnostics_t* out);

/**
 * @brief Emit a full fault dump over the log backend.
 *
 * @details
 * Logs the exception frame + diagnostics at ERROR level, then calls
 * `internal_ra_fatal_error()` which halts. Meant to be called from a
 * naked assembly trampoline installed in place of the weak
 * `HardFault_Handler` alias -- see the example in `ra_exception.c`.
 *
 * @param[in] frame       Pointer to stacked exception frame.
 * @param[in] exc_number  Exception number (2 = NMI, 3 = HardFault, ...).
 */
void ra_exception_report(const ra_exception_frame_t* frame, uint32_t exc_number)
  __attribute__((noreturn));

#ifdef __cplusplus
}
#endif
