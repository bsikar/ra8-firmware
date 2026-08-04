/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file libs/ra8_board_ra8p1/boot/nmi_exception.c
 * @brief RA8D2/RA8P1 NMI handler -- record the ICU cause instead of a bare trap
 *
 * @par Tag
 * [Ring 1 / Boot] {World: S}
 *
 * @note RA8P1 board layer (issue #226): this chip-boot TU is byte-identical to
 *       the EK-RA8D2 copy. The RA8P1 (R7KA8P1KFLCAC) shares the RA8D2 core,
 *       ICU, and register map (see libs/ra8_core/inc/ra8_device.h), so the NMI
 *       handling is common; the HUM citations reference the RA8D2 manual
 *       (R01UH1065EJ), which documents the byte-identical NMISR register.
 *
 * @details
 * The RA8D2 funnels its non-maskable interrupt sources through the
 * ICU: IWDT/WDT underflow or refresh error, the PVD1/PVD2 voltage
 * monitors, main/sub oscillation-stop detection, the NMI pin, bus
 * errors, common/local memory (SRAM ECC) errors, CPU lockup, FPU
 * exception, MRAM MRC/MRE read errors, and the inter-processor NMI.
 * Which sources reach the core is chosen in NMIER; the latched cause
 * lives in NMISR.
 *
 * Historically every per-app vector_table.c weak-aliased `NMI_Handler`
 * to `Default_Handler` -- a bare `bkpt` + `wfi` trap -- so the single
 * most safety-relevant interrupt on the chip (the watchdog biting, an
 * ECC error, a brown-out) produced zero diagnostics. The strong
 * handler in this file overrides that weak alias tree-wide, exactly
 * the way the shared `SecureFault_Handler` does:
 *
 * 1. A naked trampoline picks the stack pointer the NMI interrupted
 * (MSP if EXC_RETURN[2] = 0, PSP otherwise) and tail-calls
 * `ra8_board_nmi_report()`.
 * 2. That reads the ICU NMISR cause register and forwards frame +
 * cause into `ra8_exception_report_nmi()`, which snapshots both --
 * plus the SCB diagnostics -- into the fixed-SRAM
 * `g_ra8_exception_last` record, best-effort logs them, and parks
 * the CPU at a named halt symbol.
 *
 * NMISR is deliberately NOT acknowledged (no NMICLR write): the
 * handler never returns, and leaving the status latched preserves the
 * evidence for a post-mortem debugger attach.
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_exception.h"
#include "ra8_icu_regs.h"

/**
 * @brief Read the ICU NMI cause and hand off to the common fault record.
 *
 * @details
 * Runs as the C half of the NMI trampoline below (external linkage is
 * required for the asm tail-branch to name it). Reads NMISR exactly
 * once and forwards it, with the interrupted context's stacked frame,
 * into `ra8_exception_report_nmi()`. The register is read here -- not
 * in libs/ra8_core -- because NMISR is an RA8D2 ICU register, outside
 * the ARM-architectural scope of the core exception module.
 *
 * @param[in] frame Stacked exception frame the NMI entry pushed
 *                  (selected MSP/PSP resident); forwarded verbatim.
 *
 * @return Never returns.
 *
 * @pre The Cortex-M85 has taken the NMI exception (IPSR == 2).
 * @pre The ICU register block at 0x40006000 is accessible (always on).
 *
 * @post `g_ra8_exception_last` carries exc_number 2 plus the NMISR cause.
 * @post Control never returns; the CPU halts at a named symbol.
 *
 * @note Not thread-safe (single NMI context by construction).
 *
 * @see ra8_exception_report_nmi()  The recording + halt path.
 *
 * @since 0.1.0
 */
RA8_INTERNAL [[noreturn, gnu::used]] static void
ra8_board_nmi_report(const ra8_exception_frame_t* frame);
RA8_INTERNAL [[noreturn, gnu::used]] static void
ra8_board_nmi_report(const ra8_exception_frame_t* frame)
{
  /* HUM Ch 14.2.13 "NMISR : Non-Maskable Interrupt Status Register" p 536-540 */
  const uint32_t nmisr = *ra8_icu_nmisr();
  ra8_exception_report_nmi(frame, nmisr);
}

/**
 * @brief Cortex-M85 NMI handler (vector table slot 2).
 *
 * @details
 * Strong definition overriding the weak `NMI_Handler` alias in every
 * per-app vector_table.c. Naked trampoline in the exact shape of the
 * HardFault/MemManage/BusFault/UsageFault/SecureFault trampolines:
 * `tst lr, #4` selects the interrupted stack (MSP when EXC_RETURN[2]
 * = 0, else PSP) into `r0` and control tail-branches into
 * `ra8_board_nmi_report()`, which appends the ICU cause. Naked asm is
 * required: only assembly can read EXC_RETURN and the banked stack
 * pointers before the compiler touches any register.
 *
 * @pre The Cortex-M85 has taken the NMI exception.
 * @pre The C runtime stack selected by EXC_RETURN[2] holds the frame.
 *
 * @post Control transfers to `ra8_board_nmi_report()` and never returns.
 * @post The interrupted context's frame pointer is in `r0`.
 *
 * @since 0.1.0
 */
void                          NMI_Handler(void);
[[gnu::naked, noreturn]] void NMI_Handler(void)
{
  __asm__ volatile("tst lr, #4          \n"
                   "ite eq              \n"
                   "mrseq r0, msp       \n"
                   "mrsne r0, psp       \n"
                   "b     ra8_board_nmi_report\n");
}
