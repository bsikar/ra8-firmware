/**
 * @file libs/ra8_board_ek_ra8d2/boot/secure_exception.c
 * @brief Secure-side fault handler for NS -> S violations
 *
 * @par Tag
 * [Ring 1 / Boot] {World: S}
 *
 * @details
 * When a Non-Secure caller tries to read a Secure address (or call a
 * Secure function that has not been exposed via a NSC veneer), the
 * Cortex-M85 raises a SecureFault. The strong handler in this file
 * overrides the weak `SecureFault_Handler` alias every per-app
 * vector_table.c declares, so the whole tree decodes the fault the
 * same way the HardFault / MemManage / BusFault / UsageFault
 * trampolines do:
 *
 * 1. A naked trampoline picks the stack pointer the fault was taken
 * on (MSP if EXC_RETURN[2] = 0, PSP otherwise) and tail-calls
 * `ra8_exception_report()` with exception number 7.
 * 2. The common path snapshots the stacked frame plus the SCB
 * diagnostics -- including SFSR (why the security check fired:
 * INVEP, INVTRAN, AUVIOL, ...) and SFAR (the offending address,
 * valid when SFSR.SFARVALID) -- into the fixed-SRAM
 * `g_ra8_exception_last` record, best-effort logs every field,
 * and parks the CPU at a named halt symbol.
 *
 * `SystemInit()` sets SHCSR.SECUREFAULTENA, so security violations
 * arrive here with their true class instead of escalating to an
 * anonymous HardFault. The handler runs in the Secure world; NS code
 * cannot reach the SFSR register at 0xE000EDE4 because it lives in
 * the Secure-only system control region (RAZ from NS).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_exception.h"

/**
 * @brief Cortex-M85 SecureFault handler (vector table slot 7).
 *
 * @details
 * Strong definition overriding the weak `SecureFault_Handler` alias in
 * every per-app vector_table.c. Naked trampoline in the exact shape of
 * the HardFault/MemManage/BusFault/UsageFault trampolines: `tst lr, #4`
 * selects the faulting stack (MSP when EXC_RETURN[2] = 0, else PSP),
 * `r1` carries the architectural exception number 7, and control
 * tail-branches into `ra8_exception_report()`, which snapshots the frame
 * + SFSR/SFAR and halts. Naked asm is required: only assembly can read
 * EXC_RETURN and the banked stack pointers before the compiler touches
 * any register.
 *
 * @pre The Cortex-M85 has taken a SecureFault (SHCSR.SECUREFAULTENA=1).
 * @pre The C runtime stack selected by EXC_RETURN[2] holds the frame.
 *
 * @post Control transfers to `ra8_exception_report()` and never returns.
 * @post `g_ra8_exception_last` carries exc_number 7 plus the SFSR/SFAR pair.
 *
 * @par TrustZone Safety:
 * - **Validates:** nothing -- this is an exception entry point.
 * - **Trusts:** the SFSR/SFAR snapshot belongs to the failing access.
 * - **Denies:** any return path. The handler does not RET.
 *
 * @since 0.1.0
 */
void                          SecureFault_Handler(void);
[[gnu::naked, noreturn]] void SecureFault_Handler(void)
{
  __asm__ volatile("tst lr, #4          \n"
                   "ite eq              \n"
                   "mrseq r0, msp       \n"
                   "mrsne r0, psp       \n"
                   "mov   r1, #7        \n"
                   "b     ra8_exception_report\n");
}
