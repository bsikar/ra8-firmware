//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The fault record layouts, shared by the exception path that writes them
//! and the crash log that persists them.
//!
//! These are the wire format of a post-mortem. A debugger attached after a
//! field fault reads `g_ra8_exception_last` by symbol name with no code
//! running, and the crash-log record embeds the same struct across a reset,
//! so the field order here is a compatibility surface: it matches
//! `inc/ra8_exception.h` exactly and every struct is `extern`.

/// Stacked registers the Cortex-M exception entry pushes.
///
/// The basic, non-FP frame of Arm ARM B1.5.6. The FP variant adds S0..S15 and
/// FPSCR; only the core GPRs are recorded because FPU state is rarely the
/// root cause and would make the dump three times larger.
pub const Frame = extern struct {
    r0: u32,
    r1: u32,
    r2: u32,
    r3: u32,
    r12: u32,
    lr: u32,
    pc: u32,
    xpsr: u32,
};

/// SCB fault-status snapshot as the exception record carries it.
///
/// BFAR precedes MMFAR here, which is the opposite of the SCB register order
/// in `ra8_scb_fault_status_t`. The capture path maps one onto the other
/// field by field rather than copying the struct whole.
pub const Diagnostics = extern struct {
    cfsr: u32,
    hfsr: u32,
    dfsr: u32,
    bfar: u32,
    mmfar: u32,
    afsr: u32,
    sfsr: u32,
    sfar: u32,
};

/// The fixed-SRAM snapshot of the most recent fault or NMI.
pub const Last = extern struct {
    magic: u32,
    exc_number: u32,
    frame: Frame,
    diag: Diagnostics,
    frame_ptr: usize,
    nmisr: u32,
};

/// Sentinel written LAST, so a reader can tell a complete record from one a
/// secondary fault interrupted half-written.
pub const magic = struct {
    pub const valid: u32 = 0xFA17_DEAD;
};

/// Architectural exception numbers this module special-cases.
pub const exc = struct {
    /// NMI vector slot, which carries an ICU NMISR cause.
    pub const nmi: u32 = 2;
};

/// Map the SCB register order onto the record order.
pub fn diagnosticsFrom(
    cfsr: u32,
    hfsr: u32,
    dfsr: u32,
    mmfar: u32,
    bfar: u32,
    afsr: u32,
    sfsr: u32,
    sfar: u32,
) Diagnostics {
    return .{
        .cfsr = cfsr,
        .hfsr = hfsr,
        .dfsr = dfsr,
        .bfar = bfar,
        .mmfar = mmfar,
        .afsr = afsr,
        .sfsr = sfsr,
        .sfar = sfar,
    };
}
