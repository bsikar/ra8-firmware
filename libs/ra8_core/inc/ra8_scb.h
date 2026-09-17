/**
 * @file ra8_scb.h
 * @brief Cortex-M85 System Control Block: VTOR relocation + fault-status decode.
 * @ingroup grp_core
 *
 * @details
 * One abstraction over the Arm v8-M System Control Block (PPB window
 * 0xE000ED00), the block three first-party translation units otherwise reach
 * raw -- the exception decoder, the DFU copy-to-run launcher, and the ITM log
 * transport:
 *
 *   - **fault-status decode** -- ::ra8_scb_read_fault_status fills one struct
 *     from CFSR / HFSR / DFSR / MMFAR / BFAR / AFSR plus the ARMv8-M Security
 *     Extension SFSR / SFAR pair, exactly the set the exception decoder needs;
 *   - **vector-table relocation** -- ::ra8_scb_set_vtor / ::ra8_scb_get_vtor
 *     write and read VTOR, the primitive the DFU copy-to-run launcher uses to
 *     point the core at a freshly-staged image;
 *   - **trace unlock** -- ::ra8_scb_trace_enabled / ::ra8_scb_trace_enable read
 *     and set DEMCR.TRCENA, the bit the ITM log transport pre-checks before it
 *     touches any trace register (a clear TRCENA powers the ITM block down, so
 *     reading its registers first would bus-fault).
 *
 * This module lives in ra8_core -- the foundation layer -- rather than ra8_hal
 * because two of its three consumers (the exception decoder and the log
 * transport) are themselves ra8_core translation units, and ra8_core may depend
 * on nothing above itself (`scripts/checks/check_core_layering.py`). The SCB is
 * a Cortex-M85 core block, not an RA8D2 peripheral, so it is at home at the
 * foundation next to the exception and log code that already read it.
 *
 * These are Arm-architecture registers in the Cortex-M85 core, NOT RA8D2
 * peripherals, so the implementation references the Arm v8-M Architecture
 * Reference Manual ("Arm v8-M ARM") rather than the Renesas Hardware User's
 * Manual -- the HUM itself defers the SCB to the Arm v8-M ARM. SFSR / SFAR are
 * banked to the Secure state: read from Secure (every boot here) they carry the
 * real SecureFault cause and address, read from Non-secure they are
 * architecturally RAZ, so an unconditional capture is safe either way.
 *
 * @note On a host build (`RA8_OFF_TARGET`) the SCB window is backed by the fake
 *       MMIO map (`tests/mocks/src/ra8_fake_mmap.c`), so every read and write is
 *       observable to unit tests while having no real core effect; the decode
 *       and relocation logic are exercised exactly as on silicon.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"

/**
 * @struct ra8_scb_fault_status_t
 * @brief One snapshot of the Cortex-M85 SCB fault-status registers.
 *
 * @details
 * Mirrors the eight registers a fault decoder reads to attribute a
 * HardFault / MemManage / BusFault / UsageFault / SecureFault. Every field is
 * the verbatim 32-bit register value at the moment ::ra8_scb_read_fault_status
 * ran; interpretation of the individual cause bits is left to the caller.
 *
 * `sfsr` / `sfar` are the ARMv8-M Security Extension pair, banked to the Secure
 * state: from the Secure world they carry the SecureFault cause bits (INVEP,
 * INVTRAN, AUVIOL, ...) and the faulting address, from Non-secure they read
 * as zero. `bfar` / `mmfar` / `sfar` are only meaningful when the matching
 * "address valid" bit in `cfsr` / `sfsr` is set.
 *
 * @invariant Populated only by ::ra8_scb_read_fault_status; every field holds a
 *            raw register value (no field is a derived / decoded quantity).
 *
 * @code
 * ra8_scb_fault_status_t fs = {};
 * if (ra8_scb_read_fault_status(&fs) == k_ra8_ok) {
 *   // fs.cfsr, fs.hfsr, ... now hold the live SCB fault state.
 * }
 * @endcode
 *
 * @see ra8_scb_read_fault_status
 * @since 0.1.0
 */
typedef struct {
  uint32_t cfsr;  /**< Configurable Fault Status Register (MMFSR|BFSR|UFSR). */
  uint32_t hfsr;  /**< HardFault Status Register.                            */
  uint32_t dfsr;  /**< Debug Fault Status Register.                          */
  uint32_t mmfar; /**< MemManage Fault Address Register (if CFSR.MMARVALID). */
  uint32_t bfar;  /**< BusFault Address Register (if CFSR.BFARVALID).        */
  uint32_t afsr;  /**< Auxiliary Fault Status Register.                      */
  uint32_t sfsr;  /**< SecureFault Status Register (Secure-banked, RAZ NS).  */
  uint32_t sfar;  /**< SecureFault Address Register (if SFSR.SFARVALID).     */
} ra8_scb_fault_status_t;

/**
 * @brief Read the Cortex-M85 SCB fault-status registers into one struct.
 *
 * @details
 * Loads CFSR, HFSR, DFSR, MMFAR, BFAR, AFSR, and the Secure-banked SFSR / SFAR
 * pair with plain volatile reads and no side effects -- no register is cleared
 * or written. The read order matches the field order of
 * ::ra8_scb_fault_status_t. Safe to call from a fault context: the SCB window
 * is always mapped and the reads cannot themselves fault.
 *
 * @param[out] out Destination snapshot. Must not be `nullptr`; every field is
 *                 overwritten on success.
 *
 * @return Error code.
 * @retval k_ra8_ok           Snapshot populated from the live SCB.
 * @retval k_ra8_err_null_ptr @p out was `nullptr`; nothing was read.
 *
 * @pre @p out points to writable storage for one ::ra8_scb_fault_status_t.
 * @pre The SCB PPB window (0xE000ED00) is accessible (always on Cortex-M85; the
 *      fake MMIO map backs it on a host build).
 * @post On `k_ra8_ok` every field of `*out` reflects the SCB at call time.
 * @post No SCB register is modified (pure read).
 *
 * @note Not reentrant against a concurrent SCB write, but the reads themselves
 *       are side-effect-free and fault-context safe.
 * @since 0.1.0
 */
ra8_err_t ra8_scb_read_fault_status(ra8_scb_fault_status_t* out);

/**
 * @brief Relocate the vector table by writing SCB->VTOR.
 *
 * @details
 * Writes @p base to the Vector Table Offset Register so subsequent exception
 * and interrupt entries fetch their vectors from a table based at @p base. The
 * hardware ignores the low table-alignment bits of VTOR; the caller is
 * responsible for basing the table at a suitably aligned address (Arm v8-M
 * requires alignment to the table size, at least 128 bytes). The write is
 * issued as-is with no alignment rejection so it substitutes exactly for a raw
 * VTOR poke; ordering against a following table fetch is the caller's concern
 * (bracket with DSB/ISB before branching to the relocated image).
 *
 * @param[in] base Absolute base address of the new vector table.
 *
 * @pre @p base is aligned as the Arm v8-M ARM requires for VTOR.
 * @pre Caller has masked interrupts if a table swap must be atomic w.r.t. entry.
 * @post SCB->VTOR holds @p base (low reserved bits per hardware).
 * @post No other SCB register is modified.
 *
 * @note Not thread-safe; intended for single-threaded boot / launch context.
 * @since 0.1.0
 */
void ra8_scb_set_vtor(uintptr_t base);

/**
 * @brief Read the current vector-table base from SCB->VTOR.
 *
 * @details
 * Returns the raw VTOR value as an address. Reset value is 0 (table at the
 * start of code memory) until a boot stage or launcher relocates it.
 *
 * @return Current vector-table base address held in VTOR.
 * @retval 0..UINTPTR_MAX Whatever VTOR currently holds.
 *
 * @pre The SCB PPB window is accessible (always on Cortex-M85).
 * @pre No precondition on prior relocation -- VTOR always reads its live value.
 * @post No architectural state is modified (pure read).
 * @post The returned value equals the last value written to VTOR by hardware
 *       reset or software.
 *
 * @note Thread-safe; reads a single architectural register.
 * @since 0.1.0
 */
[[nodiscard]] uintptr_t ra8_scb_get_vtor(void);

/**
 * @brief Report whether DEMCR.TRCENA (trace subsystem enable) is set.
 *
 * @details
 * Reads bit 24 (TRCENA) of the Debug Exception and Monitor Control Register.
 * While TRCENA is clear the ITM / DWT trace block is powered down and reading
 * any of its registers bus-faults, so a trace transport must pre-check this
 * before touching ITM. DEMCR itself is always accessible.
 *
 * @return Whether the trace subsystem is enabled.
 * @retval true  DEMCR.TRCENA is set; ITM / DWT registers are safe to touch.
 * @retval false DEMCR.TRCENA is clear; the trace block is powered down.
 *
 * @pre The SCB PPB window is accessible (always on Cortex-M85).
 * @pre No precondition on debugger attach -- DEMCR reads regardless.
 * @post No architectural state is modified (pure read).
 * @post The result reflects DEMCR at the moment of the call.
 *
 * @note Thread-safe; reads a single architectural register.
 * @since 0.1.0
 */
[[nodiscard]] bool ra8_scb_trace_enabled(void);

/**
 * @brief Set DEMCR.TRCENA to power up the trace subsystem.
 *
 * @details
 * Sets bit 24 (TRCENA) of DEMCR, leaving every other bit unchanged, so the
 * ITM / DWT trace block is powered and its registers become accessible. This is
 * the unlock a firmware-driven trace transport performs when it cannot rely on
 * a debugger having set TRCENA. Idempotent: setting an already-set bit is a
 * no-op in effect.
 *
 * @pre The SCB PPB window is accessible (always on Cortex-M85).
 * @pre Caller intends to use ITM / DWT after this returns.
 * @post DEMCR.TRCENA is set; all other DEMCR bits are preserved.
 * @post ::ra8_scb_trace_enabled returns `true` afterwards.
 *
 * @note Not thread-safe against a concurrent DEMCR writer; intended for boot.
 * @since 0.1.0
 */
void ra8_scb_trace_enable(void);

#ifdef __cplusplus
}
#endif
