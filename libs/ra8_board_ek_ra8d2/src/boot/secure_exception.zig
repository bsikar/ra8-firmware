//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Secure-side fault handler for NS -> S violations. [Ring 1 / Boot] {World: S}
//!
//! When a Non-Secure caller reads a Secure address, or calls a Secure
//! function that no NSC veneer exposes, the core raises a SecureFault. The
//! strong `SecureFault_Handler` here overrides the weak alias of
//! Default_Handler that vector_table.c declares, so the fault is decoded the
//! same way the HardFault, MemManage, BusFault and UsageFault trampolines do:
//!
//! 1. The naked trampoline picks the stack the fault was taken on (MSP when
//!    EXC_RETURN[2] is 0, PSP otherwise) and tail-calls
//!    `ra8_exception_report()` with exception number 7.
//! 2. The common path snapshots the stacked frame and the SCB diagnostics,
//!    SFSR (why the security check fired) and SFAR (the offending address,
//!    valid when SFSR.SFARVALID) included, into `g_ra8_exception_last`, logs
//!    it, and parks the CPU at a named halt symbol.
//!
//! `SystemInit()` sets SHCSR.SECUREFAULTENA, so violations arrive here with
//! their true class instead of escalating to an anonymous HardFault.
//!
//! Built as its own object for each app's core, never as part of the board
//! archive: an archive member cannot beat the weak alias already in the link,
//! and an app-local src/secure_exception.c still replaces this unit
//! (cmake/ra8_app/sources.cmake, RA8FW-616). Ported from secure_exception.c
//! (RA8FW-615).

/// The trampoline. Naked, so no prologue touches the stack before the frame
/// pointer is taken; the tail call never returns.
fn secureFaultHandler() callconv(.naked) noreturn {
    asm volatile (
        \\tst lr, #4
        \\ite eq
        \\mrseq r0, msp
        \\mrsne r0, psp
        \\movs r1, #7
        \\b ra8_exception_report
    );
}

comptime {
    @export(&secureFaultHandler, .{ .name = "SecureFault_Handler" });
}
