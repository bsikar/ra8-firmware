//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! RA8D2 NMI handler: record the ICU cause instead of a bare trap.
//! [Ring 1 / Boot] {World: S}
//!
//! The chip routes its non-maskable sources through the ICU: IWDT/WDT
//! underflow or refresh error, the PVD1/PVD2 voltage monitors, oscillation
//! stop, the NMI pin, bus errors, SRAM ECC errors, CPU lockup, FPU exception,
//! MRAM read errors and the inter-processor NMI. NMIER picks which reach the
//! core; NMISR latches the cause.
//!
//! The strong `NMI_Handler` here overrides the weak alias of Default_Handler
//! that vector_table.c declares:
//!
//! 1. The naked trampoline picks the stack the NMI interrupted (MSP when
//!    EXC_RETURN[2] is 0, PSP otherwise) and tail-calls the report helper.
//! 2. The helper reads NMISR once and hands frame and cause to
//!    `ra8_exception_report_nmi()`, which snapshots both plus the SCB
//!    diagnostics into `g_ra8_exception_last`, logs them, and parks the CPU.
//!
//! NMISR is never acknowledged (no NMICLR write): the handler never returns,
//! and the latched status is evidence for a debugger attach.
//!
//! Built as its own object for each app's core, never as part of the board
//! archive, so an app-local src/nmi_exception.c still replaces it
//! (cmake/ra8_app/sources.cmake, RA8FW-616). Ported from nmi_exception.c
//! (RA8FW-620).

/// R_ICU block base (HUM Ch 14). Same value as k_ra8_icu_base_addr.
const icu_base: usize = 0x4000_6000;
/// NMISR offset from the ICU base (HUM Ch 14.2.13). Same as k_ra8_icu_off_nmisr.
const off_nmisr: usize = 0x6120;
const nmisr: *const volatile u32 = @ptrFromInt(icu_base + off_nmisr);

extern fn ra8_exception_report_nmi(frame: ?*const anyopaque, cause: u32) callconv(.c) noreturn;

/// Second half of the trampoline: read the cause once and record it.
fn nmiReport(frame: ?*const anyopaque) callconv(.c) noreturn {
    ra8_exception_report_nmi(frame, nmisr.*);
}

/// The trampoline. Naked, so no prologue touches a register before the frame
/// pointer is taken; the tail call never returns.
fn nmiHandler() callconv(.naked) noreturn {
    asm volatile (
        \\tst lr, #4
        \\ite eq
        \\mrseq r0, msp
        \\mrsne r0, psp
        \\b internal_ra8_board_nmi_report
    );
}

comptime {
    @export(&nmiReport, .{ .name = "internal_ra8_board_nmi_report", .visibility = .hidden });
    @export(&nmiHandler, .{ .name = "NMI_Handler" });
}
